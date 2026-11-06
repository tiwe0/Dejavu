#!/usr/bin/env python3
"""Rooted-device RPC regressions using synthetic data and in-memory state only."""

from __future__ import annotations

import argparse
import importlib.machinery
import importlib.util
import os
from pathlib import Path
import secrets
import socket
import sys
import threading
import time
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
loader = importlib.machinery.SourceFileLoader("device_dejavuctl", str(ROOT / "zygisk/scripts/dejavuctl"))
spec = importlib.util.spec_from_loader(loader.name, loader)
ctl = importlib.util.module_from_spec(spec)
loader.exec_module(ctl)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--process", default="com.android.settings")
    parser.add_argument("--adb", default="adb")
    args = parser.parse_args()
    os.environ["ADB"] = args.adb
    key = "_dejavu_rpc_test_" + secrets.token_hex(8)
    with ctl.ControlSession(args.serial, args.process, 40, 8) as session:
        def execute(script, timeout=40):
            original = session.timeout
            session.timeout = timeout
            try:
                status, payload = ctl.execute_with_reconnect(session, script.encode())
                assert status == ctl.RPC_OK, payload.decode(errors="replace")
                return payload
            finally:
                session.timeout = original

        assert execute("return 'RPC_READY'") == b"RPC_READY"
        try:
            # Deliver and commit a real Lua mutation, then drop its reply in the
            # caller. The CLI must recover only for the next distinct command.
            original_read = ctl.read_frame
            def lost_reply(connection, maximum_size, deadline=None):
                result = original_read(connection, maximum_size, deadline)
                if maximum_size > 0:
                    raise socket.timeout("synthetic lost reply after Agent commit")
                return result
            with patch.object(ctl, "read_frame", side_effect=lost_reply):
                try:
                    execute(f"{key}=({key} or 0)+1; return {key}")
                except ctl.OutcomeUnknownError:
                    pass
                else:
                    raise AssertionError("lost reply did not report unknown outcome")
            assert execute(f"return {key}") == b"1", "mutation replayed"
            print("OK: committed mutation with lost reply executes once; next command recovers")

            # A continuously trickling authentication frame must not reset the
            # companion's total two-second handshake budget.
            slow = socket.create_connection(("127.0.0.1", session.port), timeout=5)
            done = threading.Event()
            sent = threading.Event()
            def trickle():
                try:
                    for byte in ctl.FRAME_HEADER.pack(len(session.token)) + session.token:
                        if done.is_set():
                            break
                        slow.sendall(bytes([byte]))
                        sent.set()
                        if done.wait(0.15):
                            break
                except OSError:
                    pass
            worker = threading.Thread(target=trickle, daemon=True)
            worker.start()
            try:
                assert sent.wait(2), "slow authentication never started"
                started = time.monotonic()
                assert execute("return 'AFTER_SLOW_PEER'", timeout=6) == b"AFTER_SLOW_PEER"
                assert time.monotonic() - started < 6, "slow peer held the control loop"
            finally:
                done.set()
                slow.close()
                worker.join(2)
            assert not worker.is_alive(), "trickle worker did not finish"
            print("OK: slow authentication expires; healthy RPC proceeds")

            # This is within the documented per-wait limit and must work using
            # the default response budget (previous clients expired at 8/10s).
            script = (
                'local h=hook.stream_listen("127.0.0.1:18775",1); '
                'assert(type(h)=="number" and h>0); '
                'local ok,res=pcall(hook.stream_accept,h,10000); '
                'if ok and type(res)=="number" and res>0 then hook.stream_close(res) end; '
                'hook.stream_close(h); assert(ok,tostring(res)); '
                'assert(res==0,"expected accept timeout, got "..tostring(res)); return "WAIT_OK"'
            )
            started = time.monotonic()
            assert execute(script) == b"WAIT_OK"
            assert time.monotonic() - started >= 9, "accept did not actually wait"
            assert execute("return 'RPC_DONE'") == b"RPC_DONE"
            print("OK: supported 10-second wait completes with default client budget")
        finally:
            primary_error = sys.exc_info()[0] is not None
            try:
                execute(f"{key}=nil; return true")
            except (ctl.ControlError, OSError, AssertionError) as cleanup_error:
                if not primary_error:
                    raise
                print(f"RPC test-state cleanup failed: {cleanup_error}", file=sys.stderr)
    print("PASS: real-device RPC reliability; no message data collected")


if __name__ == "__main__":
    main()
