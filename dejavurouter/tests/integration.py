#!/usr/bin/env python3
"""Integration check for dejavurouter: hub -> C daemon -> fake DejaVu plugin."""
from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CONNECTOR = ROOT / "out/ghost-runtime/tools/x_phone_connector"
sys.path.insert(0, str(CONNECTOR))
import mock_hub  # noqa: E402


class FakePlugin(threading.Thread):
    def __init__(self, port, token, route):
        super().__init__(daemon=True)
        self.port = port
        self.token = token
        self.route = route
        self.sock = None
        self.stop = threading.Event()

    def run(self):
        self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        self.sock.settimeout(30)
        self._send({"type": "hello", "token": self.token, "app": "com.example.demo",
                    "plugin": "fake", "version": 1, "routes": [self.route], "accounts": []})
        reader = self.sock.makefile("rb")
        hello = json.loads(reader.readline())
        assert hello.get("type") == "hello_ok", hello
        while not self.stop.is_set():
            line = reader.readline()
            if not line:
                return
            message = json.loads(line)
            if message.get("type") == "request":
                self._send({"type": "response", "id": message["id"], "ok": True,
                            "result": {"echo": message.get("method"), "params": message.get("params")}})

    def _send(self, payload):
        self.sock.sendall(json.dumps(payload, separators=(",", ":")).encode() + b"\n")

    def close(self):
        self.stop.set()
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass


def main() -> int:
    daemon = Path(os.environ.get("ROUTER_BIN", str(ROOT / "dejavurouter" / "dejavurouter")))
    if not daemon.exists():
        raise SystemExit("build first: make")
    hub = mock_hub.serve("127.0.0.1", 0, "dev-test", "admin-test")
    hub_port = hub.server_address[1]
    plugin_port = 39131
    with tempfile.TemporaryDirectory() as temp:
        config = Path(temp) / "config.json"
        config.write_text(json.dumps({
            "hub_url": "http://127.0.0.1:%d" % hub_port,
            "device_id": "c-router-test",
            "device_token": "dev-test",
            "listen_host": "127.0.0.1",
            "listen_port": plugin_port,
            "plugin_token": "plugin-test",
            "poll_wait_seconds": 2,
            "plugin_wait_seconds": 3,
            "routes": {"demo": {"app": "com.example.demo", "wake": "true", "plugin_wait_seconds": 3}},
        }))
        process = subprocess.Popen([str(daemon), str(config)], stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True)
        plugin = FakePlugin(plugin_port, "plugin-test", "demo")
        try:
            for _ in range(50):
                try:
                    probe = socket.create_connection(("127.0.0.1", plugin_port), timeout=0.2)
                    probe.close()
                    break
                except OSError:
                    time.sleep(0.1)
            plugin.start()
            time.sleep(0.5)
            jobs = int(os.environ.get("JOBS", "1"))
            job_ids = []
            for index in range(jobs):
                request = urllib.request.Request(
                    "http://127.0.0.1:%d/admin/jobs" % hub_port,
                    data=json.dumps({"device_id": "c-router-test", "route": "demo",
                                     "method": "demo.echo", "params": {"hello": index},
                                     "deadline_ms": 5000}).encode(),
                    headers={"Authorization": "Bearer admin-test", "Content-Type": "application/json"},
                    method="POST")
                with urllib.request.urlopen(request, timeout=5) as response:
                    job_ids.append(json.loads(response.read())["job"]["id"])
            deadline = time.time() + 20
            completed = 0
            for job_id in job_ids:
                while time.time() < deadline:
                    try:
                        result_request = urllib.request.Request(
                            "http://127.0.0.1:%d/admin/results/%s" % (hub_port, job_id),
                            headers={"Authorization": "Bearer admin-test"})
                        with urllib.request.urlopen(result_request, timeout=2) as response:
                            payload = json.loads(response.read())
                        if not payload.get("ok"):
                            print("failed result", payload)
                            return 1
                        if payload["result"].get("echo") != "demo.echo":
                            print("wrong result", payload)
                            return 1
                        completed += 1
                        break
                    except Exception:
                        time.sleep(0.05)
                else:
                    print("timeout waiting for", job_id)
                    return 1
            print("ok", json.dumps({"route": "demo", "jobs": completed}))
            return 0
        finally:
            plugin.close()
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
            hub.shutdown()
            hub.server_close()


if __name__ == "__main__":
    raise SystemExit(main())