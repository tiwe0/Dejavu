#!/usr/bin/env python3
"""Host regressions for CLI execution deadlines and MCP forwarding."""

import importlib.machinery
import importlib.util
import io
import os
from pathlib import Path
import signal
import sys
import tempfile
import time
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import dejavu_mcp  # noqa: E402

LOADER = importlib.machinery.SourceFileLoader("dejavu_cli", str(ROOT / "dejavu"))
SPEC = importlib.util.spec_from_loader(LOADER.name, LOADER)
cli = importlib.util.module_from_spec(SPEC)
LOADER.exec_module(cli)


class ExecTimeoutTest(unittest.TestCase):
    def forwarded_command(self, *arguments):
        args = cli.build_parser().parse_args(["exec", "-p", "example.app", *arguments])
        with mock.patch.object(cli, "toolchain_env", return_value={}), \
                mock.patch.object(cli.subprocess, "run") as run:
            run.return_value.returncode = 0
            self.assertEqual(cli.command_exec(args), 0)
        return run.call_args.args[0]

    def test_custom_timeout_reaches_rpc_for_inline_and_file_scripts(self):
        for source in (("-e", "return true"), ("script.lua",)):
            with self.subTest(source=source):
                command = self.forwarded_command("--timeout", "60.5", *source)
                index = command.index("--timeout")
                self.assertEqual(command[index + 1], "60.5")

    def test_cli_default_allows_server_execution_window(self):
        command = self.forwarded_command("-e", "return true")
        index = command.index("--timeout")
        self.assertEqual(float(command[index + 1]), 40.0)

    def test_cli_rejects_nonpositive_or_nonfinite_timeouts(self):
        for value in ("0", "-1", "nan", "inf", "-inf"):
            with self.subTest(value=value), mock.patch("sys.stderr", new_callable=io.StringIO):
                with self.assertRaises(SystemExit):
                    cli.build_parser().parse_args([
                        "exec", f"--timeout={value}", "-e", "return true",
                    ])

    def test_mcp_timeout_reaches_rpc_and_leaves_cleanup_budget(self):
        class ForwardingRunner:
            timeout = 120.0

            def run(self, command, timeout=None):
                self.command = command
                self.outer_timeout = timeout
                args = cli.build_parser().parse_args(command)
                self.exit_code = cli.command_exec(args)
                return "done"

        for timeout in (60.5, None):
            with self.subTest(timeout=timeout):
                runner = ForwardingRunner()
                server = dejavu_mcp.McpServer(runner, default_process="example.app")
                arguments = {"code": "return true"}
                if timeout is not None:
                    arguments["timeout"] = timeout
                with mock.patch.object(cli, "toolchain_env", return_value={}), \
                        mock.patch.object(cli.subprocess, "run") as run:
                    run.return_value.returncode = 0
                    self.assertEqual(server.call_tool("dejavu_exec", arguments), "done")
                command = run.call_args.args[0]
                actual_timeout = float(command[command.index("--timeout") + 1])
                self.assertEqual(actual_timeout, timeout if timeout is not None else 120.0)
                self.assertGreater(runner.outer_timeout, actual_timeout)
                self.assertEqual(runner.exit_code, 0)

    def test_mcp_rejects_invalid_timeouts_before_spawning(self):
        runner = mock.Mock(timeout=120.0)
        runner.run.return_value = "unexpected call"
        server = dejavu_mcp.McpServer(runner)
        for value in (True, False, 0, -1, float("nan"), float("inf"), "60"):
            with self.subTest(value=value):
                response = server.handle_request({
                    "jsonrpc": "2.0", "id": 1, "method": "tools/call",
                    "params": {"name": "dejavu_exec", "arguments": {
                        "code": "return true", "timeout": value,
                    }},
                })
                self.assertTrue(response["result"]["isError"])
        runner.run.assert_not_called()

    def test_mcp_startup_rejects_invalid_timeouts(self):
        for value in ("0", "-1", "nan", "inf", "-inf"):
            for parser, prefix in ((cli.build_parser(), ["mcp"]),
                                   (dejavu_mcp.build_parser(), [])):
                with self.subTest(value=value, prefix=prefix), \
                        mock.patch("sys.stderr", new_callable=io.StringIO):
                    with self.assertRaises(SystemExit):
                        parser.parse_args([*prefix, f"--timeout={value}"])


class PluginTrustLockTest(unittest.TestCase):
    def test_clients_contend_and_nested_updates_release_after_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            fake_adb = Path(directory) / "adb"
            lock_path = str(Path(directory) / "trust.lock")
            fake_adb.write_text(
                "#!/usr/bin/env python3\n"
                "import fcntl, sys\n"
                f"lock = open({lock_path!r}, 'a')\n"
                "try: fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)\n"
                "except BlockingIOError:\n"
                " print('Plugin trust configuration is busy', flush=True); sys.exit(1)\n"
                "print('DEJAVU_PLUGIN_TRUST_READY', flush=True)\n"
                "sys.stdin.buffer.read()\n"
            )
            fake_adb.chmod(0o700)
            first = cli.build_parser().parse_args(["--adb", str(fake_adb), "status"])
            second = cli.build_parser().parse_args(["--adb", str(fake_adb), "status"])
            with mock.patch.dict(os.environ, {"ADB": str(fake_adb)}):
                with self.assertRaisesRegex(ValueError, "test operation"):
                    with cli.plugin_trust_lock(first):
                        with cli.plugin_trust_lock(first):
                            with self.assertRaisesRegex(cli.CliError, "busy"):
                                with cli.plugin_trust_lock(second):
                                    self.fail("second writer acquired a held lock")
                        raise ValueError("test operation")
                with cli.plugin_trust_lock(second):
                    self.assertTrue(second._plugin_trust_lock_held)
                self.assertFalse(first._plugin_trust_lock_held)
                self.assertFalse(second._plugin_trust_lock_held)


class RunnerTimeoutTest(unittest.TestCase):
    def test_timeout_stops_cli_descendants(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            heartbeat = root / "heartbeat"
            child_pid = root / "child.pid"
            child = root / "child.py"
            child.write_text(
                "import time\n"
                "from pathlib import Path\n"
                f"with Path({str(heartbeat)!r}).open('a', buffering=1) as stream:\n"
                "    while True:\n"
                "        stream.write('alive\\n')\n"
                "        time.sleep(0.02)\n"
            )
            fake_cli = root / "cli.py"
            fake_cli.write_text(
                "import subprocess, sys, time\n"
                "from pathlib import Path\n"
                f"child = subprocess.Popen([sys.executable, {str(child)!r}])\n"
                f"Path({str(child_pid)!r}).write_text(str(child.pid))\n"
                "time.sleep(60)\n"
            )
            runner = dejavu_mcp.DejavuRunner(cli_path=fake_cli, timeout=1.0)
            try:
                with self.assertRaisesRegex(dejavu_mcp.ToolFailure, "timed out"):
                    runner.run(["exec"])
                self.assertTrue(heartbeat.exists(), "descendant did not start")
                size = heartbeat.stat().st_size
                time.sleep(0.15)
                self.assertEqual(heartbeat.stat().st_size, size,
                                 "CLI descendant survived the MCP timeout")
            finally:
                if child_pid.exists():
                    try:
                        os.kill(int(child_pid.read_text()), signal.SIGKILL)
                    except ProcessLookupError:
                        pass

    def test_runner_preserves_output_and_command_errors(self):
        with tempfile.TemporaryDirectory() as directory:
            fake_cli = Path(directory) / "cli.py"
            fake_cli.write_text("print('result')\n")
            runner = dejavu_mcp.DejavuRunner(cli_path=fake_cli)
            self.assertEqual(runner.run(["exec"]), "result")
            fake_cli.write_text("import sys\nprint('bad', file=sys.stderr)\nsys.exit(2)\n")
            with self.assertRaisesRegex(dejavu_mcp.ToolFailure, "bad"):
                runner.run(["exec"])


if __name__ == "__main__":
    unittest.main()
