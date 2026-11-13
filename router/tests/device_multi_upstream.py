#!/usr/bin/env python3
"""Isolated Android multi-Hub regression using synthetic plugins and payloads.

Does not install a module, change live Router configuration, launch apps or reboot.
"""
import argparse
import json
from pathlib import Path
import secrets
import shlex
import socket
import subprocess
import tempfile
import time


class Peer:
    def __init__(self, sock):
        self.sock = sock
        sock.settimeout(6)
        self.stream = sock.makefile("rb")

    def send(self, message):
        self.sock.sendall(json.dumps(message, separators=(",", ":")).encode() + b"\n")

    def recv(self):
        line = self.stream.readline(1048578)
        assert line.endswith(b"\n") and len(line) <= 1048577, "Invalid wire frame"
        return json.loads(line)

    def expect(self, kind, caller_id=None):
        for _ in range(1024):
            message = self.recv()
            if message.get("type") == kind and (caller_id is None or message.get("id") == caller_id):
                return message
        raise AssertionError("Expected response was not received")

    def close(self):
        self.stream.close()
        self.sock.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--serial", required=True)
    parser.add_argument("--binary", required=True, type=Path)
    args = parser.parse_args()
    adb = [args.adb, "-s", args.serial]
    suffix = secrets.token_hex(6)
    directory = "/data/local/tmp/dejavu-multihub-" + suffix
    binary = directory + "/dejavuerouter"
    pid = start_time = None
    peers, listeners, forwards, reverses, uploads = [], [], [], [], []

    def run(*command):
        return subprocess.check_output(adb + list(command), text=True, timeout=30).strip()

    def root(script):
        return run("shell", "su -M -c " + shlex.quote(script))

    def private_file(local_path, relative):
        # ADB runs as shell and cannot write inside the root-only daemon state.
        staging = directory + ".upload-" + secrets.token_hex(4)
        uploads.append(staging)
        run("push", str(local_path), staging)
        target = directory + "/" + relative
        root("umask 077; mkdir -p " + shlex.quote(str(Path(target).parent)) + " && cp " +
             shlex.quote(staging) + " " + shlex.quote(target) + " && chmod 600 " +
             shlex.quote(target) + " && rm " + shlex.quote(staging))

    try:
        assert root("id -u") == "0"
        boot_id = root("cat /proc/sys/kernel/random/boot_id")
        # New temporary daemon owns only these unique sockets and private files.
        run("shell", "mkdir -p " + shlex.quote(directory))
        run("push", str(args.binary.resolve()), binary)
        root("chown 0:0 " + shlex.quote(directory) + " " + shlex.quote(binary) +
             " && chmod 700 " + shlex.quote(directory) + " " + shlex.quote(binary))
        for _ in range(3):
            listener = socket.socket()
            listener.bind(("127.0.0.1", 0))
            listener.listen(8)
            listener.settimeout(8)
            listeners.append(listener)
            for attempt in range(30):
                remote_port = 45000 + secrets.randbelow(15000)
                try:
                    run("reverse", "--no-rebind", "tcp:" + str(remote_port),
                        "tcp:" + str(listener.getsockname()[1]))
                    reverses.append(remote_port)
                    break
                except subprocess.CalledProcessError:
                    if attempt == 29:
                        raise
        plugin_socket = "dejavu.multihub.plugins." + suffix
        control_socket = "dejavu.multihub.control." + suffix
        with tempfile.TemporaryDirectory(prefix="dejavu-multihub-") as temporary:
            staging = Path(temporary)

            def upload_json(relative, value):
                local = staging / Path(relative).name
                local.write_text(json.dumps(value))
                private_file(local, relative)

            upload_json("config.json", dict(hub_host="127.0.0.1", hub_port=reverses[0],
                        device_id="device-multi-default", device_token="synthetic-default-token",
                        plugins_dir=directory + "/plugins", control_socket=control_socket,
                        plugin_socket=plugin_socket, listen_tcp=False))
            upload_json("plugins/device_multi/manifest.json", dict(schemaVersion=1,
                        id="device_multi", name="Synthetic multi-Hub", version="1", kind="lua",
                        entry="main.lua", router="router.json", files=["main.lua", "router.json"]))
            routing = dict(schemaVersion=1, services=["device_multi"], upstreams=[
                dict(id="main", host="127.0.0.1", port=reverses[1], device_token="synthetic-main-token"),
                dict(id="other", host="127.0.0.1", port=reverses[2], device_token="synthetic-other-token"),
            ], default_upstream="main", routes={"synthetic.a": "main", "synthetic.b": "other"})
            upload_json("plugins/device_multi/router.json", routing)
            local_entry = staging / "main.lua"
            local_entry.write_text("return {}\n")
            private_file(local_entry, "plugins/device_multi/main.lua")
            # Read PID start time as well as executable before stopping in cleanup.
            pid = int(root("setsid " + shlex.quote(binary) + " " + shlex.quote(directory + "/config.json") +
                           " </dev/null >/dev/null 2>&1 & echo $!"))
            start_time = root("awk '{print $22}' /proc/" + str(pid) + "/stat")
            for attempt in range(60):
                try:
                    metadata = json.loads(root("cat " + shlex.quote(directory + "/control.json") + " 2>/dev/null"))
                    break
                except (subprocess.CalledProcessError, ValueError):
                    if attempt == 59:
                        raise AssertionError("Temporary Router failed to start")
                    time.sleep(.1)
            for name in (control_socket, plugin_socket):
                forwards.append(int(run("forward", "tcp:0", "localabstract:" + name)))

            def ctl(operation="status", **fields):
                peer = Peer(socket.create_connection(("127.0.0.1", forwards[0]), timeout=5))
                try:
                    peer.send(dict(token=metadata["token"], op=operation, **fields))
                    reply = peer.recv()
                    assert reply.get("ok"), reply.get("error", "Control operation failed")
                    text = json.dumps(reply)
                    assert metadata["token"] not in text and "synthetic-default-token" not in text
                    assert "synthetic-main-token" not in text and "synthetic-other-token" not in text
                    return reply["status"]
                finally:
                    peer.close()

            hubs = []
            for index, listener in enumerate(listeners):
                peer = Peer(listener.accept()[0])
                peers.append(peer)
                hello = peer.expect("hello")
                assert hello["token"] == ("synthetic-default-token", "synthetic-main-token", "synthetic-other-token")[index]
                hubs.append(peer)
            status = ctl()
            assert {row["id"] for row in status["upstreams"]} == {"default", "device_multi/main", "device_multi/other"}

            def plugin(name, routes):
                peer = Peer(socket.create_connection(("127.0.0.1", forwards[1]), timeout=5))
                peers.append(peer)
                peer.send(dict(type="hello", app="org.example.synthetic", plugin=name, routes=routes))
                peer.expect("hello_ok")
                return peer

            custom = plugin("device_multi", ["synthetic.a", "synthetic.b"])
            legacy = plugin("legacy_without_config", ["synthetic.legacy"])
            for hub, route in zip(hubs, ("synthetic.legacy", "synthetic.a", "synthetic.b")):
                assert hub.expect("route")["route"] == route

            # Same caller ID on three upstreams must create independent jobs.
            for hub, route in zip(hubs, ("synthetic.legacy", "synthetic.a", "synthetic.b")):
                hub.send(dict(type="request", id="shared", route=route, method="echo", params={}, deadline_ms=10000))
            jobs = [legacy.expect("request"), custom.expect("request"), custom.expect("request")]
            assert len({job["id"] for job in jobs}) == 3
            for job in reversed(jobs):
                (legacy if job["route"].endswith("legacy") else custom).send(dict(
                    type="response", id=job["id"], route=job["route"], ok=True, result=job["route"]))
            for hub, route in zip(hubs, ("synthetic.legacy", "synthetic.a", "synthetic.b")):
                response = hub.expect("response", "shared")
                assert response["ok"] and response["result"] == route

            # A cancel from main must not cancel a matching ID on other.
            for hub, route in zip(hubs[1:], ("synthetic.a", "synthetic.b")):
                hub.send(dict(type="request", id="cancel-shared", route=route, method="echo", params={}, deadline_ms=10000))
            pending = {job["route"]: job for job in (custom.expect("request"), custom.expect("request"))}
            hubs[1].send(dict(type="cancel", id="cancel-shared", route="synthetic.a"))
            assert custom.expect("cancel")["id"] == pending["synthetic.a"]["id"]
            custom.send(dict(type="response", id=pending["synthetic.b"]["id"], route="synthetic.b", ok=True, result="survived"))
            assert hubs[2].expect("response", "cancel-shared")["result"] == "survived"

            # Reconnect one upstream while the other has work; do not replay it.
            hubs[2].send(dict(type="request", id="during-reconnect", route="synthetic.b", method="echo", params={}, deadline_ms=10000))
            pending_other = custom.expect("request")
            ctl("reconnect", upstream_id="device_multi/main")
            replacement = Peer(listeners[1].accept()[0])
            peers.append(replacement)
            replacement.expect("hello")
            assert replacement.expect("route")["route"] == "synthetic.a"
            custom.send(dict(type="response", id=pending_other["id"], route="synthetic.b", ok=True, result="connected"))
            assert hubs[2].expect("response", "during-reconnect")["result"] == "connected"

            # A Hub cannot use a route belonging to another upstream.
            hubs[2].send(dict(type="request", id="wrong-hub", route="synthetic.a", method="echo", params={}))
            assert hubs[2].expect("response", "wrong-hub")["ok"] is False
            # Manager's override operations retain the package credential when
            # omitted, and resetting restores the original connection settings.
            snapshot = ctl("upstream_configure", upstream_id="device_multi/main",
                           hub_host="127.0.0.1", hub_port=reverses[1], device_id="local-override")
            assert next(row for row in snapshot["upstreams"] if row["id"] == "device_multi/main")["overridden"]
            overridden = Peer(listeners[1].accept()[0])
            peers.append(overridden)
            hello = overridden.expect("hello")
            assert hello["device_id"] == "local-override" and hello["token"] == "synthetic-main-token"
            overridden.expect("route")
            snapshot = ctl("upstream_reset", upstream_id="device_multi/main")
            assert not next(row for row in snapshot["upstreams"] if row["id"] == "device_multi/main")["overridden"]
            restored = Peer(listeners[1].accept()[0])
            peers.append(restored)
            hello = restored.expect("hello")
            assert hello["device_id"] == "device-multi-default" and hello["token"] == "synthetic-main-token"
            restored.expect("route")
            ctl("reload_plugins")
            assert ctl()["pending_jobs"] == 0
            assert root("cat /proc/sys/kernel/random/boot_id") == boot_id, "Device rebooted during regression"
            print("Android multi-upstream: default fallback, plugin discovery, route assignment, ID/cancel/reconnect isolation, local overrides/reset and secret redaction passed")
    finally:
        for peer in peers:
            peer.close()
        for listener in listeners:
            listener.close()
        if pid is not None and start_time:
            root("if [ \"$(readlink /proc/" + str(pid) + "/exe)\" = " + shlex.quote(binary) +
                 " ] && [ \"$(awk '{print $22}' /proc/" + str(pid) + "/stat)\" = " + shlex.quote(start_time) +
                 " ]; then kill " + str(pid) + "; fi")
        for port in forwards:
            run("forward", "--remove", "tcp:" + str(port))
        for port in reverses:
            run("reverse", "--remove", "tcp:" + str(port))
        root("rm -rf " + shlex.quote(directory))
        for staging in uploads:
            root("rm -f " + shlex.quote(staging))


if __name__ == "__main__":
    main()
