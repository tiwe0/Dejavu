#!/usr/bin/env python3
"""End-to-end: ordinary RouterHub.call() -> dejavurouter -> fake plugin."""
from __future__ import annotations

import json
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from hub_poc import RouterHub  # noqa: E402


class FakePlugin(threading.Thread):
    def __init__(self, port: int):
        super().__init__(daemon=True)
        self.port = port
        self.stop = threading.Event()

    def run(self):
        deadline = time.time() + 5
        sock = None
        while time.time() < deadline:
            try:
                sock = socket.create_connection(("127.0.0.1", self.port), timeout=0.2)
                break
            except OSError:
                time.sleep(0.02)
        if sock is None:
            return
        sock.settimeout(None)
        sock.sendall(b'{"type":"hello","app":"com.example","routes":["demo"]}\n')
        reader = sock.makefile("rb")
        while not self.stop.is_set():
            line = reader.readline()
            if not line:
                return
            request = json.loads(line)
            if request.get("type") == "request":
                response = {"type": "response", "id": request["id"],
                            "route": request["route"], "ok": True,
                            "result": {"echo": request.get("method"),
                                       "params": request.get("params")}}
                sock.sendall(json.dumps(response).encode() + b"\n")
        sock.close()


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def main() -> int:
    daemon = ROOT / "dejavurouter"
    if not daemon.exists():
        raise SystemExit("build first: make")
    hub_port = free_port()
    plugin_port = free_port()
    hub = RouterHub(port=hub_port).start()
    config = {
        "hub_host": "127.0.0.1",
        "hub_port": hub_port,
        "device_id": "e2e-phone",
        "device_token": "e2e",
        "listen_host": "127.0.0.1",
        "listen_port": plugin_port,
    }
    with tempfile.TemporaryDirectory() as temp:
        config_path = Path(temp) / "config.json"
        config_path.write_text(json.dumps(config))
        process = subprocess.Popen([str(daemon), str(config_path)],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        plugin = FakePlugin(plugin_port)
        plugin.start()
        try:
            assert hub.wait_connected(5), "dejavurouter did not connect to hub"
            plugin_thread_ready = True
            for _ in range(50):
                try:
                    # Only the fake plugin uses a socket; the upper-level test
                    # below uses the ordinary hub.call() API.
                    probe = socket.create_connection(("127.0.0.1", plugin_port), timeout=0.1)
                    probe.close()
                    break
                except OSError:
                    time.sleep(0.05)
            time.sleep(0.6)
            result = hub.call("demo", "demo.echo", {"hello": "world"}, timeout=5)
            assert result == {"echo": "demo.echo", "params": {"hello": "world"}}, result
            raw = hub.call_raw("demo", "demo.echo", {"again": True}, timeout=5)
            assert raw["ok"] is True and raw["route"] == "demo", raw
            print("ok hub_poc e2e:", json.dumps(result, sort_keys=True))
            return 0
        finally:
            plugin.stop.set()
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
            hub.stop()


if __name__ == "__main__":
    raise SystemExit(main())