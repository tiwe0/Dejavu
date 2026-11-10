#!/usr/bin/env python3
"""Tests for the socket-hiding hub PoC facade."""
from __future__ import annotations

import json
import socket
import sys
import threading
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))

from hub_poc import CallError, CallTimeoutError, NotConnectedError, RouterHub  # noqa: E402


class FakeRouter(threading.Thread):
    def __init__(self, port: int):
        super().__init__(daemon=True)
        self.port = port
        self.sock = None
        self.requests = []

    def run(self):
        deadline = time.time() + 5
        while time.time() < deadline:
            try:
                self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=0.2)
                break
            except OSError:
                time.sleep(0.02)
        if self.sock is None:
            return
        self.sock.sendall(b'{"type":"hello","device_id":"fake-phone","version":1}\n')
        reader = self.sock.makefile("rb")
        while True:
            line = reader.readline()
            if not line:
                return
            request = json.loads(line)
            self.requests.append(request)
            if request.get("method") == "slow":
                continue
            if request.get("method") == "bad":
                response = {"type": "response", "id": request["id"],
                            "route": request["route"], "ok": False,
                            "error": {"code": "route_unavailable",
                                      "message": "no plugin", "retryable": True}}
            elif request.get("method") == "wrong_route":
                response = {"type": "response", "id": request["id"],
                            "route": "wrong", "ok": True, "result": {}}
            else:
                response = {"type": "response", "id": request["id"],
                            "route": request["route"], "ok": True,
                            "result": {"method": request.get("method"),
                                       "params": request.get("params")}}
            self.sock.sendall(json.dumps(response).encode() + b"\n")


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def main() -> int:
    port = free_port()
    hub = RouterHub(port=port).start()
    try:
        try:
            hub.call("r", "token")
            raise AssertionError("call before connect should fail")
        except NotConnectedError:
            pass

        router = FakeRouter(port)
        router.start()
        assert hub.wait_connected(5), "router did not connect"
        assert hub.device["device_id"] == "fake-phone"

        result = hub.call("route-1", "echo", {"x": 1})
        assert result == {"method": "echo", "params": {"x": 1}}, result
        assert hub.call_raw("route-2", "echo")["route"] == "route-2"

        try:
            hub.call("route-1", "bad")
            raise AssertionError("bad call should raise")
        except CallError as error:
            assert error.code == "route_unavailable"

        try:
            hub.call("route-1", "wrong_route")
            raise AssertionError("mismatched route should raise")
        except CallError as error:
            assert error.code == "route_mismatch"

        pending = hub.call_async("route-1", "echo", {"async": True})
        assert pending.result_value(5) == {"method": "echo", "params": {"async": True}}

        try:
            hub.call("route-1", "slow", timeout=0.1)
            raise AssertionError("slow call should time out")
        except CallTimeoutError:
            pass

        assert hub.stats["calls"] >= 5
        print("ok hub_poc:", hub.stats["calls"], "calls")
        return 0
    finally:
        hub.stop()


if __name__ == "__main__":
    raise SystemExit(main())
