#!/usr/bin/env python3
"""Device regression: Lua hook.stream_listen/accept/read/write roundtrip.

Requires a rooted device with the DejaVu module (v0.8.0+) installed, the target
app running, and adb reachable. Runs the Lua server inside the target process
through the dejavu CLI and connects from the host via adb forward:

    python3 zygisk/tests/device/stream_rpc_test.py --serial <serial>
"""

from __future__ import annotations

import argparse
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
LUA_TEMPLATE = Path(__file__).with_name("stream_rpc_test.lua")


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", "-s", required=True)
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--process", "-p", default="com.etsy.android")
    parser.add_argument("--port", type=int, default=18766)
    parser.add_argument("--timeout", type=float, default=40.0)
    return parser.parse_args()


def roundtrip(port, timeout=2.0):
    sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    try:
        payload = b"ping"
        sock.sendall(struct.pack(">I", len(payload)) + payload)
        header = b""
        while len(header) < 4:
            chunk = sock.recv(4 - len(header))
            if not chunk:
                raise ConnectionError("connection closed before reply")
            header += chunk
        (length,) = struct.unpack(">I", header)
        body = b""
        while len(body) < length:
            chunk = sock.recv(length - len(body))
            if not chunk:
                raise ConnectionError("connection closed during reply")
            body += chunk
        return body.decode("utf-8", "replace")
    finally:
        sock.close()


def main() -> int:
    args = parse_args()
    adb = [args.adb, "-s", args.serial]
    script = LUA_TEMPLATE.read_text().replace("__PORT__", str(args.port))
    with tempfile.NamedTemporaryFile("w", suffix=".lua", delete=False) as handle:
        handle.write(script)
        lua_path = handle.name
    subprocess.run(adb + ["forward", "--remove", "tcp:%d" % args.port],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(adb + ["forward", "tcp:%d" % args.port, "tcp:%d" % args.port],
                   check=True, stdout=subprocess.DEVNULL)
    executor = subprocess.Popen(
        [sys.executable, str(ROOT / "dejavu"), "--adb", args.adb, "-s", args.serial,
         "exec", "-p", args.process, "--timeout", str(int(args.timeout)), lua_path],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    reply = None
    last_error = None
    try:
        deadline = time.monotonic() + 12.0
        while time.monotonic() < deadline:
            try:
                reply = roundtrip(args.port)
                break
            except (OSError, ConnectionError) as error:
                last_error = error
                time.sleep(0.25)
        try:
            stdout, stderr = executor.communicate(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            executor.kill()
            stdout, stderr = executor.communicate()
        if stderr.strip():
            print(stderr.strip(), file=sys.stderr)
        if reply is None:
            print("FAIL: host client could not complete roundtrip: %s" % last_error,
                  file=sys.stderr)
            print("lua:", stdout.strip(), file=sys.stderr)
            return 1
        print("reply:", reply)
        print("lua:", stdout.strip())
        if executor.returncode != 0:
            print("FAIL: dejavu exec rc=%d" % executor.returncode, file=sys.stderr)
            return 1
        if reply != "pong:ping":
            print("FAIL: reply mismatch", file=sys.stderr)
            return 1
        if "OK" not in stdout:
            print("FAIL: Lua result did not report OK", file=sys.stderr)
            return 1
        print("PASS: device-side hook.stream_listen/accept/read/write roundtrip")
        return 0
    finally:
        if executor.poll() is None:
            executor.kill()
            executor.wait(timeout=5)
        subprocess.run(adb + ["forward", "--remove", "tcp:%d" % args.port],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        Path(lua_path).unlink(missing_ok=True)


if __name__ == "__main__":
    raise SystemExit(main())
