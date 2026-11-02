#!/usr/bin/env python3
"""Exercise TCP accept/reconnect and fragmented stream records on the host."""

import argparse
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("receiver", type=Path)
    args = parser.parse_args()
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    process = subprocess.Popen(
        [str(args.receiver.resolve()), "--transport", "tcp", "--bind", "127.0.0.1",
         "--port", str(port)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    try:
        deadline = time.monotonic() + 5
        while True:
            try:
                connection = socket.create_connection(("127.0.0.1", port), timeout=0.2)
                break
            except OSError:
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise AssertionError("stream receiver did not start")
                time.sleep(0.02)
        with connection:
            frame = struct.pack("!I", 5) + b"hello"
            connection.sendall(frame[:2])
            connection.sendall(frame[2:6])
            connection.sendall(frame[6:] + struct.pack("!I", 0))
        with socket.create_connection(("127.0.0.1", port), timeout=2) as connection:
            payload = "second 中文".encode()
            connection.sendall(struct.pack("!I", len(payload)) + payload)
        expected = b"hello\n\nsecond " + "中文".encode() + b"\n"
        output = b""
        deadline = time.monotonic() + 5
        while len(output) < len(expected):
            remaining = deadline - time.monotonic()
            assert remaining > 0, "timed out reading receiver output"
            ready, _, _ = select.select([process.stdout], [], [], remaining)
            assert ready, "receiver did not output records"
            chunk = os.read(process.stdout.fileno(), 4096)
            assert chunk, "receiver exited before producing all records"
            output += chunk
        assert output == expected, (output, expected)
        print("OK: TCP stream receiver accepts fragmented frames and reconnects")
    finally:
        process.terminate()
        try:
            process.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()


if __name__ == "__main__":
    main()
