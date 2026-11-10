#!/usr/bin/env python3
"""Account-table routing and disconnect/recovery tests for the hub PoC."""
from __future__ import annotations

import json
import socket
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from hub_poc import (  # noqa: E402
    AccountUnknownError,
    CallError,
    DeviceOfflineError,
    RouterHub,
    RouteOfflineError,
)


class FakeDevice(threading.Thread):
    def __init__(self, port: int, device_id: str, route: str):
        super().__init__(daemon=True)
        self.port = port
        self.device_id = device_id
        self.route = route
        self.stop = threading.Event()
        self.sock: socket.socket | None = None

    def run(self):
        sock = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        self.sock = sock
        sock.settimeout(None)
        hello = {"type": "hello", "device_id": self.device_id, "version": 1}
        sock.sendall(json.dumps(hello).encode() + b"\n")
        sock.sendall(json.dumps({
            "type": "route", "action": "register", "route": self.route,
            "device_id": self.device_id, "app": "fake.app",
        }).encode() + b"\n")
        reader = sock.makefile("rb")
        while not self.stop.is_set():
            line = reader.readline()
            if not line:
                return
            request = json.loads(line)
            if request.get("type") == "request":
                sock.sendall(json.dumps({
                    "type": "response", "id": request["id"],
                    "route": request["route"], "ok": True,
                    "result": {"device": self.device_id, "route": request["route"]},
                }).encode() + b"\n")

    def close(self):
        self.stop.set()
        if self.sock is not None:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = None


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def main() -> int:
    port = free_port()
    hub = RouterHub(port=port).start()
    try:
        first = FakeDevice(port, "phone-a", "route-acc")
        first.start()
        assert hub.wait_connected(5), "first device did not connect"
        for _ in range(50):
            if "route-acc" in hub.routes:
                break
            time.sleep(0.02)
        assert "route-acc" in hub.routes, hub.routes

        binding = hub.bind_account("acc-1", "phone-a", "route-acc")
        assert binding.route == "route-acc"
        assert hub.account_for("acc-1") == binding

        result = hub.call_account("acc-1", "echo", {"n": 1})
        assert result == {"device": "phone-a", "route": "route-acc"}, result

        try:
            hub.call_account("missing", "echo")
            raise AssertionError("unknown account should fail")
        except AccountUnknownError:
            pass

        # Disconnect: account row must survive, live route/connection must drop.
        first.close()
        first.join(timeout=2)
        deadline = time.time() + 5
        while time.time() < deadline and (hub.connected or hub.routes):
            time.sleep(0.02)
        assert not hub.connected, "hub still reports connected"
        assert not hub.routes, f"routes not cleared: {hub.routes}"
        assert "acc-1" in hub.accounts, "account table should survive disconnect"
        try:
            hub.call_account("acc-1", "echo")
            raise AssertionError("offline account call should fail")
        except (DeviceOfflineError, RouteOfflineError):
            pass

        # Reconnect: the same account row resolves again after route announce.
        second = FakeDevice(port, "phone-a", "route-acc")
        second.start()
        assert hub.wait_connected(5), "device did not reconnect"
        for _ in range(50):
            if "route-acc" in hub.routes:
                break
            time.sleep(0.02)
        assert "route-acc" in hub.routes, hub.routes
        result = hub.call_account("acc-1", "echo", {"n": 2})
        assert result == {"device": "phone-a", "route": "route-acc"}, result
        print("ok hub accounts + reconnect:", json.dumps({"accounts": list(hub.accounts),
                                                          "routes": hub.routes}))
        return 0
    finally:
        hub.stop()


if __name__ == "__main__":
    raise SystemExit(main())
