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


if __name__ == "__main__":
    unittest.main()
