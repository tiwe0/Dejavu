#!/usr/bin/env python3

import importlib.machinery
import importlib.util
import io
import os
from pathlib import Path
import socket
import sys
import tempfile
import unittest
from unittest import mock


ROOT_DIR = Path(__file__).resolve().parents[1]
SCRIPT_PATH = ROOT_DIR / "scripts" / "dejavuctl"
LOADER = importlib.machinery.SourceFileLoader("dejavuctl", str(SCRIPT_PATH))
SPEC = importlib.util.spec_from_loader(LOADER.name, LOADER)
dejavuctl = importlib.util.module_from_spec(SPEC)
LOADER.exec_module(dejavuctl)


class ParseArgumentsTest(unittest.TestCase):
    def parse(self, *argv):
        with mock.patch.object(sys, "argv", ["dejavuctl", *argv]):
            return dejavuctl.parse_arguments()

    def test_positional_script_and_process_remain_supported(self):
        arguments = self.parse("script.lua", "bin.mt.plus:worker")
        self.assertEqual(arguments.script, "script.lua")
        self.assertEqual(arguments.process_arg, "bin.mt.plus:worker")
        self.assertFalse(arguments.repl)
        self.assertIsNone(arguments.watch)

    def test_default_timeout_covers_server_execution_window(self):
        self.assertEqual(self.parse("-e", "return true").timeout, 40.0)

    def test_nonfinite_wait_and_timeout_are_rejected(self):
        for flag in ("--wait", "--timeout"):
            for value in ("nan", "inf", "-inf"):
                with self.subTest(flag=flag, value=value), \
                     mock.patch("sys.stderr", new_callable=io.StringIO):
                    with self.assertRaises(SystemExit):
                        self.parse("-e", "return true", f"{flag}={value}")

    def test_repl_accepts_positional_process(self):
        arguments = self.parse("--repl", "bin.mt.plus")
        self.assertTrue(arguments.repl)
        self.assertEqual(arguments.process_arg, "bin.mt.plus")
        self.assertIsNone(arguments.script)

    def test_watch_accepts_positional_process(self):
        arguments = self.parse("--watch", "hooks.lua", "bin.mt.plus")
        self.assertEqual(arguments.watch, "hooks.lua")
        self.assertEqual(arguments.process_arg, "bin.mt.plus")
        self.assertIsNone(arguments.script)

    def test_watch_without_positional_process_uses_default_target(self):
        arguments = self.parse("--watch", "hooks.lua")
        self.assertEqual(arguments.watch, "hooks.lua")
        self.assertIsNone(arguments.process_arg)
        self.assertIsNone(arguments.script)

    def test_interactive_sources_remain_mutually_exclusive(self):
        with mock.patch("sys.stderr", new_callable=io.StringIO):
            with self.assertRaises(SystemExit):
                self.parse("--repl", "-e", "return true")


class ScriptValidationTest(unittest.TestCase):
    def test_load_script_file_rejects_empty_script(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "empty.lua"
            path.write_bytes(b"")
            with self.assertRaisesRegex(dejavuctl.ControlError, "Lua script is empty"):
                dejavuctl.load_script_file(path)

    def test_only_connection_failures_trigger_reconnect(self):
        self.assertTrue(dejavuctl.is_reconnectable_error(dejavuctl.ControlError(
            "RPC connection closed unexpectedly"
        )))
        self.assertTrue(dejavuctl.is_reconnectable_error(OSError("adb forward failed")))
        self.assertTrue(dejavuctl.is_reconnectable_error(socket.timeout("slow")))
        self.assertFalse(dejavuctl.is_reconnectable_error(dejavuctl.ControlError(
            "execution error for pid 1: boom"
        )))

    def test_adb_metadata_commands_have_a_finite_subprocess_deadline(self):
        with mock.patch.object(dejavuctl.subprocess, "run") as run:
            dejavuctl.adb_command(None, "shell", "pidof example.app")
        self.assertEqual(run.call_args.kwargs["timeout"], 5.0)

    def test_adb_timeout_is_reported_as_control_error(self):
        with mock.patch.object(dejavuctl.subprocess, "run", side_effect=
                               dejavuctl.subprocess.TimeoutExpired("adb", 5)):
            with self.assertRaisesRegex(dejavuctl.ControlError, "adb command timed out"):
                dejavuctl.adb_command(None, "shell", "pidof example.app")

    def test_watch_atomic_save_before_read_and_lost_reply_executes_saved_version_once(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "hooks.lua"
            replacement = Path(directory) / "saved.lua"
            path.write_bytes(b"version_A()")
            replacement.write_bytes(b"version_B()")
            opened = False
            real_open = Path.open
            def open_after_save(candidate, *args, **kwargs):
                nonlocal opened
                if candidate == path and not opened:
                    opened = True
                    replacement.replace(path)
                return real_open(candidate, *args, **kwargs)
            session = mock.Mock(pid=321)
            effects = []
            def execute(script):
                effects.append(script)
                if len(effects) > 1:
                    raise KeyboardInterrupt()
                raise socket.timeout("lost reply")
            session.execute.side_effect = execute
            with mock.patch.object(Path, "open", open_after_save), \
                 mock.patch.object(dejavuctl.time, "sleep", side_effect=[None, KeyboardInterrupt()]), \
                 mock.patch("sys.stderr", new_callable=io.StringIO):
                result = dejavuctl.run_watch(session, str(path))
            self.assertEqual(result, 1)
            self.assertEqual(effects, [b"version_B()"])

    def test_watch_detects_replaced_inode_with_same_size_and_mtime(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "hooks.lua"
            replacement = Path(directory) / "saved.lua"
            path.write_bytes(b"version_A()")
            replacement.write_bytes(b"version_B()")
            before = dejavuctl.file_identity(path)
            original = path.stat()
            os.utime(replacement, ns=(original.st_atime_ns, original.st_mtime_ns))
            replacement.replace(path)
            self.assertNotEqual(dejavuctl.file_identity(path), before)

    def test_watch_changed_then_restored_original_snapshot_does_not_replay(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "hooks.lua"
            saved = Path(directory) / "original.lua"
            replacement = Path(directory) / "replacement.lua"
            path.write_bytes(b"install_A()")
            replacement.write_bytes(b"install_B()")
            session = mock.Mock(pid=321)
            effects = []
            def execute(script):
                effects.append(script)
                if len(effects) > 1:
                    raise KeyboardInterrupt()
                raise socket.timeout("lost reply")
            session.execute.side_effect = execute
            real_wait = dejavuctl.wait_for_watch_change
            waits = 0
            def observe_then_restore(candidate, identity):
                nonlocal waits
                waits += 1
                if waits > 1:
                    raise KeyboardInterrupt()
                path.rename(saved)
                replacement.rename(path)
                observed = real_wait(candidate, identity)
                saved.replace(path)
                return observed
            with mock.patch.object(dejavuctl, "wait_for_watch_change", observe_then_restore), \
                 mock.patch.object(dejavuctl.time, "sleep"), \
                 mock.patch("sys.stderr", new_callable=io.StringIO):
                self.assertEqual(dejavuctl.run_watch(session, str(path)), 1)
            self.assertEqual(effects, [b"install_A()"])

    def test_watch_missing_then_same_snapshot_does_not_replay_lost_reply(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "hooks.lua"
            path.write_bytes(b"install_hook()")
            identity = dejavuctl.file_identity(path)
            session = mock.Mock(pid=321)
            effects = []
            def execute(script):
                effects.append(script)
                if len(effects) > 1:
                    raise KeyboardInterrupt()
                raise socket.timeout("lost reply")
            session.execute.side_effect = execute
            with mock.patch.object(dejavuctl, "file_identity", side_effect=[
                    identity, OSError("temporarily missing"), identity]), \
                 mock.patch.object(dejavuctl.time, "sleep", side_effect=[
                     None, None, KeyboardInterrupt()]), \
                 mock.patch("sys.stderr", new_callable=io.StringIO):
                self.assertEqual(dejavuctl.run_watch(session, str(path)), 1)
            self.assertEqual(effects, [b"install_hook()"])

    def test_snapshot_retries_in_place_change_before_dispatch(self):
        source = mock.MagicMock()
        source.__enter__.return_value = source
        source.read.side_effect = [b"old", b"new"]
        def info(mtime):
            return mock.Mock(st_dev=1, st_ino=2, st_mtime_ns=mtime, st_size=3)
        with mock.patch.object(Path, "open", return_value=source), \
             mock.patch.object(dejavuctl.os, "fstat", side_effect=[
                 info(1), info(2), info(2), info(2)]):
            self.assertEqual(dejavuctl.load_script_snapshot("hooks.lua"),
                             (b"new", (1, 2, 2, 3)))
        self.assertEqual(source.read.call_count, 2)

    def test_snapshot_retry_budget_rejects_continuously_changing_file(self):
        source = mock.MagicMock()
        source.__enter__.return_value = source
        source.read.return_value = b"script"
        identities = [mock.Mock(st_dev=1, st_ino=2, st_mtime_ns=i, st_size=6)
                      for i in range(6)]
        with mock.patch.object(Path, "open", return_value=source), \
             mock.patch.object(dejavuctl.os, "fstat", side_effect=identities):
            with self.assertRaisesRegex(dejavuctl.ControlError, "file changed while reading"):
                dejavuctl.load_script_snapshot("hooks.lua")
        self.assertEqual(source.read.call_count, 3)

    def test_watch_change_recovers_after_temporary_stat_failure(self):
        with mock.patch.object(dejavuctl, "WATCH_POLL_INTERVAL", 0), \
            mock.patch.object(dejavuctl.time, "sleep"), \
            mock.patch.object(
                dejavuctl,
                "file_identity",
                side_effect=[OSError("gone"), (2, 3)],
            ), \
            mock.patch.object(dejavuctl, "print_error") as print_error:
            identity = dejavuctl.wait_for_watch_change(Path("hooks.lua"), (1, 1))
        self.assertEqual(identity, (2, 3))
        print_error.assert_called_once()

    def test_watch_recovery_waits_for_distinct_snapshot(self):
        with mock.patch.object(dejavuctl, "WATCH_POLL_INTERVAL", 0), \
            mock.patch.object(dejavuctl.time, "sleep"), \
            mock.patch.object(
                dejavuctl,
                "file_identity",
                side_effect=[OSError("gone"), (1, 1), (2, 2)],
            ), \
            mock.patch.object(dejavuctl, "print_error") as print_error:
            identity = dejavuctl.wait_for_watch_change(Path("hooks.lua"), (1, 1))
        self.assertEqual(identity, (2, 2))
        print_error.assert_called_once()

    def test_session_forward_cleanup_cannot_mask_execution_result(self):
        for failed_execution in (False, True):
            session = dejavuctl.ControlSession(None, "example.app", 40, 0)
            session.port, session.pid = 1234, 321
            execute_result = socket.timeout("lost reply") if failed_execution else None
            with mock.patch.object(session, "refresh_metadata"), \
                 mock.patch.object(dejavuctl, "execute_with_port", return_value=(
                     dejavuctl.RPC_OK, b"confirmed"), side_effect=execute_result) as execute, \
                 mock.patch.object(dejavuctl.subprocess, "run", side_effect=
                     dejavuctl.subprocess.TimeoutExpired("adb", 5)), \
                 mock.patch("sys.stderr", new_callable=io.StringIO) as stderr:
                if failed_execution:
                    with self.assertRaisesRegex(dejavuctl.OutcomeUnknownError, "outcome unknown"):
                        with session:
                            dejavuctl.execute_with_reconnect(session, b"install_hook()")
                else:
                    with session:
                        result = dejavuctl.execute_with_reconnect(session, b"install_hook()")
                    self.assertEqual(result, (dejavuctl.RPC_OK, b"confirmed"))
            execute.assert_called_once()
            self.assertIn("forward", stderr.getvalue())
            self.assertIn("may remain", stderr.getvalue())
            self.assertIsNone(session.port)

    def test_forwarded_port_cleanup_cannot_mask_primary_exception(self):
        with mock.patch.object(dejavuctl.subprocess, "run", side_effect=[
                mock.Mock(stdout="1234"), dejavuctl.subprocess.TimeoutExpired("adb", 5)]), \
             mock.patch("sys.stderr", new_callable=io.StringIO) as stderr:
            with self.assertRaisesRegex(dejavuctl.OutcomeUnknownError, "original lost reply"):
                with dejavuctl.forwarded_port(None, "dejavu-socket") as port:
                    self.assertEqual(port, 1234)
                    raise dejavuctl.outcome_unknown("original lost reply")
        self.assertIn("may remain", stderr.getvalue())

    def test_reconnect_with_zero_wait_still_attempts_once(self):
        session = dejavuctl.ControlSession(None, "bin.mt.plus", 10.0, 0.0)
        session.pid = 123
        with mock.patch.object(session, "close"), \
            mock.patch.object(
                session,
                "refresh_metadata",
                side_effect=OSError("metadata missing"),
            ) as refresh_metadata:
            with self.assertRaisesRegex(OSError, "metadata missing"):
                session.reconnect()
        refresh_metadata.assert_called_once_with(wait_seconds=0)

    def test_uncertain_transport_failure_never_replays_side_effect(self):
        for runner in (dejavuctl.execute_with_reconnect,
                       dejavuctl.execute_with_status_reconnect):
            for error in (socket.timeout("lost reply"),
                          dejavuctl.ControlError("RPC connection closed unexpectedly")):
                with self.subTest(runner=runner.__name__, error=error):
                    session = mock.Mock(pid=321, wait_seconds=0)
                    effects = []
                    def execute(script):
                        effects.append(script)
                        if len(effects) > 1:
                            raise AssertionError("command replayed")
                        raise error
                    session.execute.side_effect = execute
                    with self.assertRaisesRegex(dejavuctl.ControlError, "outcome unknown"):
                        runner(session, b"install_hook()")
                    self.assertEqual(effects, [b"install_hook()"])
                    session.reconnect.assert_not_called()

    def test_ambiguous_unavailable_never_replays(self):
        for runner in (dejavuctl.execute_with_reconnect,
                       dejavuctl.execute_with_status_reconnect):
            for payload in (b"target Agent disconnected; retry the command",
                            b"target Agent disconnected; execution outcome unknown"):
                session = mock.Mock(pid=321, wait_seconds=0)
                session.execute.side_effect = [(dejavuctl.RPC_UNAVAILABLE, payload),
                                               AssertionError("command replayed")]
                with self.assertRaisesRegex(dejavuctl.ControlError, "outcome unknown"):
                    runner(session, b"install_hook()")
                session.execute.assert_called_once()
                session.reconnect.assert_not_called()

    def test_definitely_not_dispatched_retries_once_and_stops(self):
        for runner in (dejavuctl.execute_with_reconnect,
                       dejavuctl.execute_with_status_reconnect):
            session = mock.Mock(pid=321, wait_seconds=0)
            session.execute.return_value = (
                dejavuctl.RPC_UNAVAILABLE, b"target Agent is not connected")
            session.execute.side_effect = [session.execute.return_value] * 2 + [
                AssertionError("retry budget exceeded")]
            with mock.patch.object(dejavuctl, "print_error"):
                result = runner(session, b"install_hook()")
            self.assertEqual(result, session.execute.return_value)
            self.assertEqual(session.execute.call_count, 2)
            session.reconnect.assert_called_once_with()

    def test_definitely_not_dispatched_can_recover(self):
        session = mock.Mock(pid=321, wait_seconds=0)
        session.execute.side_effect = [
            (dejavuctl.RPC_UNAVAILABLE, b"target Agent is not connected"),
            (dejavuctl.RPC_OK, b"true"),
        ]
        with mock.patch.object(dejavuctl, "print_error"):
            result = dejavuctl.execute_with_reconnect(session, b"return true")
        self.assertEqual(result, (dejavuctl.RPC_OK, b"true"))
        session.reconnect.assert_called_once_with()

    def test_uncertain_command_recovers_only_before_next_command(self):
        for next_pid in (321, 654):
            with self.subTest(next_pid=next_pid):
                session = dejavuctl.ControlSession(None, "example.app", 40, 0)
                session.port, session.pid = 1234, 321
                session.socket_name, session.token = "dejavu-old", b"old"
                with mock.patch.object(dejavuctl, "execute_with_port", side_effect=[
                        socket.timeout("reply lost"),
                        (dejavuctl.RPC_OK, b""),
                        (dejavuctl.RPC_OK, b"true"),
                        (dejavuctl.RPC_OK, b"next")]) as execute, \
                     mock.patch.object(dejavuctl, "wait_for_metadata", return_value=(
                         next_pid, "dejavu-replacement", b"new")) as metadata, \
                     mock.patch.object(dejavuctl, "adb_command", return_value=
                                       mock.Mock(stdout="4321")), \
                     mock.patch("sys.stderr", new_callable=io.StringIO):
                    with self.assertRaisesRegex(dejavuctl.ControlError, "outcome unknown"):
                        dejavuctl.execute_with_reconnect(session, b"install_hook()")
                    metadata.assert_not_called()
                    self.assertTrue(session.reconnect_pending)
                    self.assertEqual(session.execute(b"inspect_state()"),
                                     (dejavuctl.RPC_OK, b"next"))
                self.assertFalse(session.reconnect_pending)
                self.assertEqual((session.pid, session.socket_name, session.port),
                                 (next_pid, "dejavu-replacement", 4321))
                self.assertEqual([call.args[4] for call in execute.call_args_list],
                                 [b"install_hook()", b"", b"return true", b"inspect_state()"])
                self.assertEqual(execute.call_args.args[:2], (4321, b"new"))

    def test_slow_fragments_share_one_exchange_deadline(self):
        clock = [0.0]
        connection = mock.Mock()
        def recv(_size):
            clock[0] += 0.04
            return b"x"
        connection.recv.side_effect = recv
        with mock.patch.object(dejavuctl.time, "monotonic", side_effect=lambda: clock[0]):
            with self.assertRaisesRegex(socket.timeout, "deadline"):
                dejavuctl.read_exact(connection, 10, deadline=0.1)
        self.assertEqual(connection.recv.call_count, 3)
        self.assertEqual(connection.settimeout.call_count, 3)
        self.assertLess(connection.settimeout.call_args.args[0], 0.021)

    def test_frame_header_and_body_use_same_deadline(self):
        connection = mock.Mock()
        clock = [0.0]
        def recv(_size):
            clock[0] += 0.06
            return dejavuctl.FRAME_HEADER.pack(1) if clock[0] < 0.1 else b"x"
        connection.recv.side_effect = recv
        with mock.patch.object(dejavuctl.time, "monotonic", side_effect=lambda: clock[0]):
            with self.assertRaisesRegex(socket.timeout, "deadline"):
                dejavuctl.read_frame(connection, 1, deadline=0.1)
        self.assertEqual(connection.settimeout.call_count, 2)
        self.assertAlmostEqual(connection.settimeout.call_args.args[0], 0.04)

    def test_partial_send_and_lost_reply_are_outcome_unknown(self):
        for partial_send in (True, False):
            connection = mock.MagicMock()
            connection.__enter__.return_value = connection
            connection.sendall.side_effect = [None, socket.timeout("partial")] if partial_send else None
            with mock.patch.object(dejavuctl.socket, "create_connection", return_value=connection), \
                 mock.patch.object(dejavuctl, "read_frame", side_effect=[b"", socket.timeout("reply")]):
                with self.assertRaisesRegex(dejavuctl.ControlError, "outcome unknown"):
                    dejavuctl.execute_once(1234, b"token", 1, 2, b"install_hook()", 40)
            self.assertEqual(connection.sendall.call_count, 2)


class ReplTests(unittest.TestCase):
    def parse(self, *args):
        with mock.patch.object(sys, "argv", ["dejavuctl", *args]):
            return dejavuctl.parse_arguments()

    def test_parse_repl_options(self):
        arguments = self.parse("--repl", "-p", "com.demo.app", "--no-color", "--history", "/tmp/hist")
        self.assertTrue(arguments.repl)
        self.assertEqual(arguments.process_option, "com.demo.app")
        self.assertTrue(arguments.no_color)
        self.assertEqual(arguments.history, "/tmp/hist")

    def test_lua_long_string_escapes_brackets_and_leading_newline(self):
        self.assertEqual(dejavuctl.lua_long_string("hello"), "[[\nhello]]")
        self.assertEqual(dejavuctl.lua_long_string("a]]b"), "[=[\na]]b]=]")
        self.assertEqual(dejavuctl.lua_long_string("ends]"), "[=[\nends]]=]")
        self.assertEqual(dejavuctl.lua_long_string("a\r\nb"), "[[\na\nb]]")

    def test_derive_class_alias(self):
        self.assertEqual(dejavuctl.derive_class_alias("java.lang.StringBuilder"), "StringBuilder")
        self.assertEqual(
            dejavuctl.derive_class_alias("android.app.ActivityThread$AppBindData"),
            "AppBindData",
        )
        self.assertEqual(dejavuctl.derive_class_alias("42:com.demo.Helper"), "Helper")
        with self.assertRaises(dejavuctl.ControlError):
            dejavuctl.derive_class_alias("com.demo.end")

    def test_split_inspect_args(self):
        self.assertEqual(dejavuctl.split_inspect_args("app"), ("app", None))
        self.assertEqual(dejavuctl.split_inspect_args("app getPackage"), ("app", "getPackage"))
        self.assertEqual(
            dejavuctl.split_inspect_args('java.use("java.lang.String") "sub"'),
            ('java.use("java.lang.String")', "sub"),
        )
        self.assertEqual(dejavuctl.split_inspect_args("java.app()"), ("java.app()", None))

    def test_run_repl_multiline_ctrl_c_and_dot_commands(self):
        executed = []
        session = mock.Mock()
        session.process_name = "com.demo.app"
        session.pid = 4321
        session.reconnect_pending = False

        def fake_execute(script):
            text = script.decode("utf-8")
            executed.append(text)
            if "type(dejavu._repl_eval)" in text:
                return dejavuctl.RPC_OK, b"true"
            if "dejavu._repl_eval(" in text:
                if "function f()\n  return 7\nend" in text:
                    return dejavuctl.RPC_OK, b'{"ok":true,"incomplete":false,"stdout":"","values":[]}'
                if "function f()" in text:
                    return dejavuctl.RPC_OK, b'{"ok":false,"incomplete":true,"stdout":"","error":"eof"}'
                if "1 +" in text:
                    return dejavuctl.RPC_OK, b'{"ok":false,"incomplete":true,"stdout":"","error":"eof"}'
                if "StringBuilder = java.use" in text:
                    return (
                        dejavuctl.RPC_OK,
                        b'{"ok":true,"incomplete":false,"stdout":"","values":["<java.class java.lang.StringBuilder>"]}',
                    )
                if "from-file" in text:
                    return (
                        dejavuctl.RPC_OK,
                        b'{"ok":true,"incomplete":false,"stdout":"loaded-ok","values":["123"]}',
                    )
                return (
                    dejavuctl.RPC_OK,
                    b'{"ok":true,"incomplete":false,"stdout":"hi","values":["42"]}',
                )
            if "dejavu._repl_gc()" in text:
                return dejavuctl.RPC_OK, b'{"memory_kb":128.5,"live_java_refs":2}'
            if "dejavu._repl_hooks()" in text:
                return (
                    dejavuctl.RPC_OK,
                    b'{"java":["1\\tactive\\tcom.demo.A#foo()V"],"native":[]}',
                )
            if "_v.__inspect" in text:
                return dejavuctl.RPC_OK, b"class com.demo.App\n  getPackageName()"
            if text == "return 999":
                return dejavuctl.RPC_OK, b"999"
            raise AssertionError(f"unexpected script: {text!r}")

        session.execute.side_effect = fake_execute

        with tempfile.TemporaryDirectory() as tmpdir:
            lua_file = Path(tmpdir) / "snippet.lua"
            lua_file.write_text("print('from-file')\nreturn 123\n", encoding="utf-8")
            inputs = [
                "1 +\n",
                KeyboardInterrupt(),
                "function f()\n",
                "  return 7\n",
                "end\n",
                "f()\n",
                ".5\n",
                ".help\n",
                ".use java.lang.StringBuilder\n",
                ".inspect app getPackage\n",
                f".load {lua_file}\n",
                ".hooks\n",
                ".gc\n",
                ".reconnect\n",
                ".raw return 999\n",
                ".quit\n",
            ]
            input_iter = iter(inputs)

            def fake_read(_prompt):
                nxt = next(input_iter)
                if isinstance(nxt, BaseException):
                    raise nxt
                return nxt

            stdout_buf = io.StringIO()
            stdout_buf.buffer = io.BytesIO()
            stdout_buf.isatty = lambda: True
            stderr_buf = io.StringIO()
            with mock.patch.object(dejavuctl, "read_command", side_effect=fake_read), \
                 mock.patch.object(dejavuctl, "readline", None), \
                 mock.patch.object(sys, "stdin", new=mock.Mock(isatty=lambda: True)), \
                 mock.patch.object(sys, "stdout", new=stdout_buf), \
                 mock.patch.object(sys, "stderr", new=stderr_buf):
                rc = dejavuctl.run_repl(session, no_color=True)

        self.assertEqual(rc, 0)
        self.assertEqual(stderr_buf.getvalue(), "")
        out = stdout_buf.getvalue()
        self.assertIn("Dejavu REPL connected to com.demo.app (pid 4321)", out)
        self.assertIn("hi", out)
        self.assertIn("42", out)
        self.assertIn("Dejavu Lua REPL commands:", out)
        self.assertIn("StringBuilder = <java.class java.lang.StringBuilder>", out)
        self.assertIn("getPackageName()", out)
        self.assertIn("loaded-ok", out)
        self.assertIn("123", out)
        self.assertIn("Java hooks (1):", out)
        self.assertIn("GC complete: 128.5 KB Lua memory, 2 live Java ref(s)", out)
        self.assertIn("Reconnected to com.demo.app (pid 4321)", out)
        self.assertEqual(stdout_buf.buffer.getvalue(), b"999\n")
        session.reconnect.assert_called_once()

    def test_non_tty_repl_does_not_swallow_keyboard_interrupt(self):
        session = mock.Mock()
        session.process_name = "com.demo.app"
        session.pid = 4321
        session.reconnect_pending = False
        session.execute.return_value = (dejavuctl.RPC_OK, b"true")
        with mock.patch.object(dejavuctl, "read_command", side_effect=KeyboardInterrupt()), \
             mock.patch.object(sys, "stdin", new=mock.Mock(isatty=lambda: False)), \
             mock.patch.object(sys, "stdout", new=io.StringIO()), \
             mock.patch.object(sys, "stderr", new=io.StringIO()):
            with self.assertRaises(KeyboardInterrupt):
                dejavuctl.run_repl(session, no_color=True)

    def test_run_repl_fallback_mode_and_eof_avoids_duplicate_rpc(self):
        session = mock.Mock()
        session.process_name = "com.demo.legacy"
        session.pid = 777
        session.reconnect_pending = False
        eval_calls = []

        def fake_execute(script):
            text = script.decode("utf-8")
            if "type(dejavu._repl_eval)" in text:
                return dejavuctl.RPC_OK, b"false"
            eval_calls.append(text)
            return dejavuctl.RPC_OK, b'{"ok":false,"incomplete":true,"stdout":"","error":"eof"}'

        session.execute.side_effect = fake_execute
        stdout_buf = io.StringIO()
        stderr_buf = io.StringIO()
        with mock.patch.object(dejavuctl, "read_command", side_effect=["function broken()\n", EOFError()]), \
             mock.patch.object(sys, "stdin", new=mock.Mock(isatty=lambda: False)), \
             mock.patch.object(sys, "stdout", new=stdout_buf), \
             mock.patch.object(sys, "stderr", new=stderr_buf):
            rc = dejavuctl.run_repl(session, no_color=True)

        self.assertEqual(rc, 1)
        self.assertEqual(len(eval_calls), 1)
        self.assertIn("eof", stderr_buf.getvalue())

    def test_repl_completer_dot_commands_and_remote_candidates(self):
        session = mock.Mock()
        session.reconnect_pending = False
        session.execute.return_value = (
            dejavuctl.RPC_OK,
            b'["app:getPackageManager","app:getPackageName"]',
        )
        runner = dejavuctl.ReplRunner(session, interactive=True)
        runner.has_native_repl = True
        completer = dejavuctl.ReplCompleter(runner)

        fake_rl = mock.Mock()
        fake_rl.get_line_buffer.return_value = ".ins"
        with mock.patch.object(dejavuctl, "readline", fake_rl):
            self.assertEqual(completer.complete(".ins", 0), ".inspect")
            self.assertIsNone(completer.complete(".ins", 1))

        fake_rl.get_line_buffer.return_value = "app:getP"
        with mock.patch.object(dejavuctl, "readline", fake_rl):
            self.assertEqual(completer.complete("app:getP", 0), "app:getPackageManager")
            self.assertEqual(completer.complete("app:getP", 1), "app:getPackageName")
            self.assertIsNone(completer.complete("app:getP", 2))

        # Must never fail or call session.execute when reconnect_pending is True
        session.execute.reset_mock()
        session.reconnect_pending = True
        with mock.patch.object(dejavuctl, "readline", fake_rl):
            self.assertIsNone(completer.complete("app:getP", 0))
        session.execute.assert_not_called()

        # Dot-commands must not complete while inside a multiline continuation
        runner.in_continuation = True
        fake_rl.get_line_buffer.return_value = ".ins"
        with mock.patch.object(dejavuctl, "readline", fake_rl):
            self.assertIsNone(completer.complete(".ins", 0))

    def test_repl_complete_uses_short_timeout_and_marks_reconnect_pending(self):
        session = mock.Mock()
        session.port = 12345
        session.token = b"\x01" * 32
        session.timeout = 40.0
        session.reconnect_pending = False
        runner = dejavuctl.ReplRunner(session, interactive=True)
        runner.has_native_repl = True

        with mock.patch.object(
            dejavuctl, "execute_with_port", side_effect=socket.timeout("hung")
        ) as exec_port:
            self.assertEqual(runner.complete_remote("app:getP"), [])
        self.assertTrue(session.reconnect_pending)
        self.assertLessEqual(exec_port.call_args.args[5], dejavuctl.REPL_COMPLETE_TIMEOUT)
        self.assertEqual(exec_port.call_args.args[6], 0.0)

    def test_repl_style_prompt_control_bytes_and_dot_command_resilience(self):
        # Never leak raw \x01/\x02 when readline is inactive or libedit
        self.assertNotIn("\x01", dejavuctl.ReplStyle(enabled=True, readline_mode="none").prompt("p> "))
        self.assertNotIn("\x01", dejavuctl.ReplStyle(enabled=True, readline_mode="libedit").prompt("p> "))
        self.assertIn("\x01", dejavuctl.ReplStyle(enabled=True, readline_mode="gnu").prompt("p> "))

        session = mock.Mock()
        session.process_name = "com.demo.app"
        session.pid = 99
        session.reconnect_pending = False
        responses = iter(
            [
                (dejavuctl.RPC_OK, b"true"),  # initial probe
                (dejavuctl.RPC_OK, b"not-json"),  # .gc malformed JSON
                (dejavuctl.RPC_OK, b'{"memory_kb":null,"live_java_refs":null}'),  # .gc nulls
                (dejavuctl.RPC_OK, b"[]"),  # .hooks non-dict JSON
            ]
        )
        session.execute.side_effect = lambda _script: next(responses)

        with tempfile.TemporaryDirectory() as tmpdir:
            bad_utf8 = Path(tmpdir) / "bad bin.lua"
            bad_utf8.write_bytes(b"\xff\xfe\xfd")
            inputs = [
                f'.load "{bad_utf8}"\n',
                ".gc\n",
                ".gc\n",
                ".hooks\n",
                ".quit\n",
            ]
            stdout_buf = io.StringIO()
            stderr_buf = io.StringIO()
            with mock.patch.object(dejavuctl, "read_command", side_effect=inputs), \
                 mock.patch.object(sys, "stdin", new=mock.Mock(isatty=lambda: False)), \
                 mock.patch.object(sys, "stdout", new=stdout_buf), \
                 mock.patch.object(sys, "stderr", new=stderr_buf):
                rc = dejavuctl.run_repl(session, no_color=True)

        self.assertEqual(rc, 1)
        self.assertIn("cannot decode", stderr_buf.getvalue())
        self.assertIn("invalid REPL response payload", stderr_buf.getvalue())
        self.assertIn("GC complete: 0.0 KB Lua memory, 0 live Java ref(s)", stdout_buf.getvalue())

    def test_repl_safety_reserved_aliases_nul_load_control_session_quick_and_history_preserve(self):
        # 1) .use rejects reserved core globals (both derived and explicit)
        with self.assertRaises(dejavuctl.ControlError):
            dejavuctl.derive_class_alias("com.example.dejavu")
        self.assertEqual(dejavuctl.derive_class_alias("[Ljava.lang.String;"), "String")

        # 2) ControlSession with port=None never triggers reconnect/refresh_metadata on Tab
        real_session = dejavuctl.ControlSession(None, "com.demo.app", 40.0, 5.0)
        real_session.port = None
        real_session.token = None
        real_session.reconnect_pending = False
        runner = dejavuctl.ReplRunner(real_session, interactive=True)
        runner.has_native_repl = True
        with mock.patch.object(real_session, "refresh_metadata") as refresh:
            self.assertEqual(runner.complete_remote("app:getP"), [])
        refresh.assert_not_called()
        self.assertTrue(real_session.reconnect_pending)

        # 3) Failed ControlSession.reconnect() marks reconnect_pending = True
        real_session.reconnect_pending = False
        real_session.wait_seconds = 0.0
        with mock.patch.object(real_session, "refresh_metadata", side_effect=dejavuctl.ControlError("down")):
            with self.assertRaises(dejavuctl.ControlError):
                real_session.reconnect()
        self.assertTrue(real_session.reconnect_pending)

        # 4) .load with embedded NUL byte and empty quotes does not crash REPL
        session = mock.Mock()
        session.process_name = "com.demo.app"
        session.pid = 77
        session.reconnect_pending = False
        session.execute.return_value = (dejavuctl.RPC_OK, b"true")
        stderr_buf = io.StringIO()
        with mock.patch.object(
            dejavuctl,
            "read_command",
            side_effect=[
                '.use java.lang.String dejavu\n',
                '.load ""\n',
                '.load /tmp/bad\x00file.lua\n',
                '.quit\n',
            ],
        ), \
             mock.patch.object(sys, "stdin", new=mock.Mock(isatty=lambda: False)), \
             mock.patch.object(sys, "stdout", new=io.StringIO()), \
             mock.patch.object(sys, "stderr", new=stderr_buf):
            rc = dejavuctl.run_repl(session, no_color=True)
        self.assertEqual(rc, 1)
        err_text = stderr_buf.getvalue()
        self.assertIn("reserved Lua alias", err_text)
        self.assertIn("usage: .load <file.lua>", err_text)
        self.assertIn("cannot read", err_text)

        # 5) configured_readline must not overwrite a foreign/unreadable history file on EINVAL
        fake_rl = mock.Mock()
        fake_rl.__doc__ = "Importing this module enables command line editing using libedit."
        fake_rl.get_completer.return_value = None
        fake_rl.get_completer_delims.return_value = " "
        fake_rl.read_history_file.side_effect = OSError(22, "Invalid argument")
        with tempfile.TemporaryDirectory() as tmpdir:
            hist = Path(tmpdir) / "gnu_history"
            hist.write_text("plain text history\n", encoding="utf-8")
            with mock.patch.object(dejavuctl, "readline", fake_rl):
                with dejavuctl.configured_readline(runner, history_path=str(hist)) as mode:
                    self.assertEqual(mode, "libedit")
        fake_rl.write_history_file.assert_not_called()

    def test_maybe_reprobe_retries_after_initial_transport_failure_and_pid_change(self):
        session = mock.Mock()
        session.process_name = "com.demo.app"
        session.pid = 100
        session.reconnect_pending = False
        calls = []

        def fake_reconnect():
            session.reconnect_pending = False
            session.pid = 200

        session.reconnect.side_effect = fake_reconnect

        def fake_execute(script):
            text = script.decode("utf-8")
            calls.append((session.pid, text))
            if len(calls) == 1:
                raise dejavuctl.OutcomeUnknownError("initial drop")
            if "type(dejavu._repl_eval)" in text:
                return dejavuctl.RPC_OK, b"true"
            return dejavuctl.RPC_OK, b'{"ok":true,"incomplete":false,"stdout":"","values":["7"]}'

        session.execute.side_effect = fake_execute
        runner = dejavuctl.ReplRunner(session, interactive=False)
        self.assertFalse(runner.probe_native_repl())
        self.assertFalse(runner.probe_succeeded)
        self.assertTrue(session.reconnect_pending)

        res = runner.eval_code("3 + 4")
        self.assertTrue(res["ok"])
        self.assertTrue(runner.has_native_repl)
        self.assertEqual(runner.probed_pid, 200)
        session.reconnect.assert_called_once()

    def test_read_metadata_reports_not_running_and_prefers_live_newest_pid(self):
        with mock.patch.object(
            dejavuctl, "root_shell", return_value=mock.Mock(returncode=1, stdout="")
        ):
            with self.assertRaisesRegex(dejavuctl.ControlError, "target process is not running"):
                dejavuctl.read_metadata(None, "com.demo.app")

        good_token = "ab" * 32
        good_json = (
            f'{{"version":1,"pid":202,"socket":"dejavu-{"12" * 16}","token":"{good_token}"}}\n'
        )

        def fake_root_shell(_serial, command, check=True):
            if command.startswith("pidof "):
                return mock.Mock(returncode=0, stdout="101 202\n")
            if command.startswith("ls -1t "):
                return mock.Mock(returncode=0, stdout="com.demo.app.202.rpc\ncom.demo.app.101.rpc\n")
            if "/proc/202/stat" in command:
                return mock.Mock(returncode=0, stdout="com.demo.app\n")
            if command.endswith("com.demo.app.202.rpc"):
                return mock.Mock(returncode=0, stdout=good_json)
            return mock.Mock(returncode=1, stdout="")

        with mock.patch.object(dejavuctl, "root_shell", side_effect=fake_root_shell):
            pid, sock, token = dejavuctl.read_metadata(None, "com.demo.app")
        self.assertEqual(pid, 202)
        self.assertEqual(sock, f"dejavu-{'12' * 16}")
        self.assertEqual(token, bytes.fromhex(good_token))

    def test_execute_with_port_fast_polls_reconnect_unavailable_message(self):
        replies = [
            (
                dejavuctl.RPC_UNAVAILABLE,
                b"target Agent is not connected; restart the target process if it stays offline",
            ),
            (dejavuctl.RPC_OK, b"Agent channel is connected"),
        ]
        with mock.patch.object(dejavuctl, "execute_once", side_effect=replies) as call_mock, \
                mock.patch.object(dejavuctl.time, "sleep"):
            status, payload = dejavuctl.execute_with_port(
                12345, b"\x01" * 32, dejavuctl.RPC_RECONNECT_REQUEST,
                dejavuctl.RPC_RECONNECT_RESPONSE, b"", 5.0, 2.0,
            )
        self.assertEqual(status, dejavuctl.RPC_OK)
        self.assertEqual(payload, b"Agent channel is connected")
        self.assertEqual(call_mock.call_count, 2)


if __name__ == "__main__":
    unittest.main()
