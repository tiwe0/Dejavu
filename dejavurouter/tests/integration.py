#!/usr/bin/env python3
"""TCP integration check: fake hub -> dejavurouter -> fake DejaVu plugin."""
from __future__ import annotations

import json
import os
import socket
import subprocess
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class FakeHub(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.conn = None
        self.reader = None
        self.ready = threading.Event()
        self.responses = {}
        self.cond = threading.Condition()

    def run(self):
        self.listener.settimeout(5)
        try:
            conn, _ = self.listener.accept()
        except OSError:
            return
        self.conn = conn
        self.reader = conn.makefile("rb")
        hello = self.reader.readline()
        if not hello:
            return
        self.ready.set()
        while True:
            line = self.reader.readline()
            if not line:
                return
            try:
                message = json.loads(line)
            except json.JSONDecodeError:
                continue
            if message.get("type") == "response":
                with self.cond:
                    self.responses[message.get("id")] = message
                    self.cond.notify_all()

    def send(self, payload):
        if not self.conn:
            raise RuntimeError("hub not connected")
        self.conn.sendall(json.dumps(payload, separators=(",", ":")).encode() + b"\n")

    def response(self, job_id, timeout=10):
        deadline = time.time() + timeout
        with self.cond:
            while job_id not in self.responses:
                remaining = deadline - time.time()
                if remaining <= 0:
                    return None
                self.cond.wait(remaining)
            return self.responses[job_id]

    def close(self):
        try:
            self.listener.close()
        except OSError:
            pass
        if self.conn:
            try:
                self.conn.close()
            except OSError:
                pass


class FakePlugin(threading.Thread):
    def __init__(self, port):
        super().__init__(daemon=True)
        self.port = port
        self.sock = None
        self.stop = threading.Event()

    def run(self):
        self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        self.sock.settimeout(30)
        self._send({"type": "hello", "app": "com.example.demo", "plugin": "fake",
                    "version": 1, "routes": ["demo"], "accounts": []})
        reader = self.sock.makefile("rb")
        hello = json.loads(reader.readline())
        assert hello.get("type") == "hello_ok", hello
        while not self.stop.is_set():
            line = reader.readline()
            if not line:
                return
            message = json.loads(line)
            if message.get("type") == "request":
                self._send({"type": "response", "id": message["id"],
                            "route": message.get("route"), "ok": True,
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
    hub = FakeHub()
    hub.start()
    plugin_port = 39131
    with tempfile.TemporaryDirectory() as temp:
        config = Path(temp) / "config.json"
        config.write_text(json.dumps({
            "hub_host": "127.0.0.1",
            "hub_port": hub.port,
            "device_id": "c-router-test",
            "device_token": "dev-test",
            "listen_host": "127.0.0.1",
            "listen_port": plugin_port,
        }))
        process = subprocess.Popen([str(daemon), str(config)], stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True)
        if not hub.ready.wait(5):
            print("hub did not receive hello")
            process.terminate()
            return 1
        for _ in range(50):
            try:
                probe = socket.create_connection(("127.0.0.1", plugin_port), timeout=0.2)
                probe.close()
                break
            except OSError:
                time.sleep(0.1)
        plugin = FakePlugin(plugin_port)
        plugin.start()
        time.sleep(0.5)
        try:
            jobs = int(os.environ.get("JOBS", "1"))
            job_ids = []
            for index in range(jobs):
                job_id = "job_%d" % index
                job_ids.append(job_id)
                hub.send({"type": "request", "id": job_id, "route": "demo",
                          "method": "demo.echo", "params": {"hello": index},
                          "deadline_ms": 5000})
            for job_id in job_ids:
                response = hub.response(job_id, timeout=10)
                if not response or not response.get("ok"):
                    print("failed response", job_id, response)
                    return 1
                if response.get("route") != "demo" or response.get("result", {}).get("echo") != "demo.echo":
                    print("wrong response", response)
                    return 1
            print("ok", json.dumps({"transport": "tcp", "route": "demo", "jobs": len(job_ids)}))
            return 0
        finally:
            plugin.close()
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
            hub.close()


if __name__ == "__main__":
    raise SystemExit(main())
