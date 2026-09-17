#!/usr/bin/env python3

import importlib.machinery
import importlib.util
import io
from pathlib import Path
import socket
import sys
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
        with mock.patch.object(Path, "read_bytes", return_value=b""):
            with self.assertRaisesRegex(dejavuctl.ControlError, "Lua script is empty"):
                dejavuctl.load_script_file("empty.lua")

    def test_only_connection_failures_trigger_reconnect(self):
        self.assertTrue(dejavuctl.is_reconnectable_error(dejavuctl.ControlError(
            "RPC connection closed unexpectedly"
        )))
        self.assertTrue(dejavuctl.is_reconnectable_error(OSError("adb forward failed")))
        self.assertTrue(dejavuctl.is_reconnectable_error(socket.timeout("slow")))
        self.assertFalse(dejavuctl.is_reconnectable_error(dejavuctl.ControlError(
            "execution error for pid 1: boom"
        )))

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

    def test_watch_change_reexecutes_when_file_recovers_without_new_mtime(self):
        with mock.patch.object(dejavuctl, "WATCH_POLL_INTERVAL", 0), \
            mock.patch.object(dejavuctl.time, "sleep"), \
            mock.patch.object(
                dejavuctl,
                "file_identity",
                side_effect=[OSError("gone"), (1, 1)],
            ), \
            mock.patch.object(dejavuctl, "print_error") as print_error:
            identity = dejavuctl.wait_for_watch_change(Path("hooks.lua"), (1, 1))
        self.assertEqual(identity, (1, 1))
        print_error.assert_called_once()

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

    def test_execute_with_reconnect_retries_until_transport_recovers(self):
        session = mock.Mock()
        session.pid = 321
        session.execute.side_effect = [
            dejavuctl.ControlError("RPC connection closed unexpectedly"),
            (dejavuctl.RPC_OK, b"true"),
        ]
        with mock.patch.object(dejavuctl, "print_error"):
            status, payload = dejavuctl.execute_with_reconnect(session, b"return true")
        self.assertEqual((status, payload), (dejavuctl.RPC_OK, b"true"))
        session.reconnect.assert_called_once_with()


if __name__ == "__main__":
    unittest.main()
