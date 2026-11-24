#!/usr/bin/env python3
"""Phone Router v2 binary protocol tests against real host dejavuerouter."""
import json
import os
from pathlib import Path
import shutil
import socket
import ssl
import struct
import subprocess
import tempfile
import time
import unittest

BINARY = Path(os.environ.get('ROUTER_BIN', 'out/router-host/dejavuerouter')).resolve()

V2_VERSION = 0x02
FRAME_HELLO = 0x01
FRAME_HELLO_ACK = 0x02
FRAME_HEARTBEAT = 0x03
FRAME_HEARTBEAT_ACK = 0x04
FRAME_ROUTE_DELTA = 0x05
FRAME_RPC_REQUEST = 0x10
FRAME_RPC_RESPONSE = 0x11
FRAME_RPC_CANCEL = 0x12
FRAME_FLOW_CONTROL = 0x13


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class JsonlPeer:
    def __init__(self, sock):
        self.sock = sock
        self.sock.settimeout(5)
        self.buffer = b''

    def send(self, value):
        self.sock.sendall(json.dumps(value, separators=(',', ':')).encode() + b'\n')

    def recv(self):
        while b'\n' not in self.buffer:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError('peer disconnected')
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b'\n', 1)
        return json.loads(line)

    def close(self):
        self.sock.close()


class V2Peer:
    def __init__(self, sock):
        self.sock = sock
        self.sock.settimeout(5)
        self.buffer = b''

    def _read_exact(self, n):
        while len(self.buffer) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError('v2 peer disconnected')
            self.buffer += chunk
        out, self.buffer = self.buffer[:n], self.buffer[n:]
        return out

    def send_frame(self, frame_type, payload_obj):
        data = json.dumps(payload_obj, separators=(',', ':')).encode('utf-8')
        header = struct.pack('!IBB', 2 + len(data), V2_VERSION, frame_type)
        self.sock.sendall(header + data)

    def recv_frame(self):
        hdr = self._read_exact(6)
        body_len, version, frame_type = struct.unpack('!IBB', hdr)
        if version != V2_VERSION:
            raise AssertionError(f'unexpected version 0x{version:02x}')
        payload = self._read_exact(body_len - 2)
        return frame_type, json.loads(payload.decode('utf-8'))

    def expect_frame(self, expected_type, skip_types=(FRAME_HEARTBEAT, FRAME_FLOW_CONTROL)):
        for _ in range(64):
            ft, payload = self.recv_frame()
            if ft == expected_type:
                return payload
            if ft in skip_types:
                if ft == FRAME_HEARTBEAT:
                    self.send_frame(FRAME_HEARTBEAT_ACK, {
                        'echoSentAtMs': payload.get('sentAtMs', 0),
                        'serverNowMs': int(time.time() * 1000),
                    })
                continue
            raise AssertionError(f'expected frame 0x{expected_type:02x}, got 0x{ft:02x}: {payload}')
        raise AssertionError(f'did not receive frame 0x{expected_type:02x}')

    def close(self):
        self.sock.close()


class RouterV2Tests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='dvr-v2-', dir='/tmp')
        self.state = Path(self.directory.name)
        self.config = self.state / 'config.json'
        self.config.write_text(json.dumps({
            'listen_tcp': False,
            'listen_port': free_port(),
            'hub_protocol': 'v2',
        }))
        self.log = open(self.state / 'test.log', 'wb')
        self.peers = []
        self.process = None
        self.start_daemon()

    def start_daemon(self):
        self.process = subprocess.Popen([str(BINARY), str(self.config)], stdout=self.log, stderr=self.log)
        metadata = self.state / 'control.json'
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.fail('daemon exited: ' + (self.state / 'test.log').read_text())
            if metadata.exists():
                self.metadata = json.loads(metadata.read_text())
                return
            time.sleep(0.01)
        self.fail('daemon did not publish control metadata')

    def tearDown(self):
        for peer in self.peers:
            peer.close()
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3)
        rc = self.process.returncode if self.process else 0
        self.log.close()
        diag = (self.state / 'test.log').read_text()
        self.directory.cleanup()
        self.assertEqual(rc, 0, diag)

    def ctl(self, op='status', **fields):
        sock = socket.socket(socket.AF_UNIX)
        sock.connect(self.metadata['name'])
        peer = JsonlPeer(sock)
        try:
            peer.send({'token': self.metadata['token'], 'op': op, **fields})
            return peer.recv()
        finally:
            peer.close()

    def connect_plugin(self, route='x_acct_1', app='com.twitter.android', service=None):
        sock = socket.socket(socket.AF_UNIX)
        sock.connect(str(self.state / 'plugins.sock'))
        peer = JsonlPeer(sock)
        self.peers.append(peer)
        hello = {'type': 'hello', 'app': app, 'routes': [route]}
        if service is not None:
            hello['plugin'] = service
        peer.send(hello)
        self.assertEqual(peer.recv()['type'], 'hello_ok')
        return peer

    def start_v2_hub(self, hb_secs=5, max_inflight=16):
        listener = socket.socket()
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(('127.0.0.1', 0))
        listener.listen(16)
        listener.settimeout(5)
        self.addCleanup(listener.close)
        resp = self.ctl('configure', hub_host='127.0.0.1', hub_port=listener.getsockname()[1],
                        device_id='phone-v2-1', device_token='v2-secret-token', hub_protocol='v2')
        self.assertTrue(resp['ok'], resp)
        conn, _ = listener.accept()
        hub = V2Peer(conn)
        self.peers.append(hub)
        ft, hello = hub.recv_frame()
        self.assertEqual(ft, FRAME_HELLO)
        self.assertEqual(hello['deviceId'], 'phone-v2-1')
        self.assertEqual(hello['token'], 'v2-secret-token')
        self.assertIn('router.v2', hello['capabilities'])
        hub.send_frame(FRAME_HELLO_ACK, {
            'ok': True,
            'heartbeatIntervalSecs': hb_secs,
            'maxInflight': max_inflight,
            'error': None,
        })
        # Immediate post-HelloAck sync heartbeat
        ft, hb = hub.recv_frame()
        self.assertEqual(ft, FRAME_HEARTBEAT)
        hub.send_frame(FRAME_HEARTBEAT_ACK, {
            'echoSentAtMs': hb['sentAtMs'],
            'serverNowMs': int(time.time() * 1000),
        })
        return hub, listener, hello

    def test_v2_auth_rejection_triggers_reconnect(self):
        listener = socket.socket()
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(('127.0.0.1', 0))
        listener.listen(16)
        listener.settimeout(5)
        self.addCleanup(listener.close)
        resp = self.ctl('configure', hub_host='127.0.0.1', hub_port=listener.getsockname()[1],
                        device_id='phone-v2-reject', device_token='bad', hub_protocol='v2')
        self.assertTrue(resp['ok'])
        conn1, _ = listener.accept()
        p1 = V2Peer(conn1)
        self.peers.append(p1)
        ft, _ = p1.recv_frame()
        self.assertEqual(ft, FRAME_HELLO)
        p1.send_frame(FRAME_HELLO_ACK, {
            'ok': False,
            'heartbeatIntervalSecs': 15,
            'maxInflight': 16,
            'error': 'unauthorized',
        })
        # Should reconnect after backoff and expose last_error / reconnect_count in status
        conn2, _ = listener.accept()
        p2 = V2Peer(conn2)
        self.peers.append(p2)
        ft, hello2 = p2.recv_frame()
        self.assertEqual(ft, FRAME_HELLO)
        self.assertEqual(hello2['deviceId'], 'phone-v2-reject')
        status = self.ctl('status')['status']
        self.assertEqual(status['last_error'], 'unauthorized')
        self.assertGreaterEqual(status['reconnect_count'], 1)

    def test_v2_multiplexed_rpc_cancel_telemetry_and_wake(self):
        hub, _, _ = self.start_v2_hub()
        status = self.ctl('status')['status']
        self.assertEqual(status['hub_protocol'], 'v2')
        self.assertTrue(status['hub_connected'])
        self.assertIsNone(status['last_error'])
        self.assertGreater(status['last_heartbeat_ack_ms'], 0)
        self.assertGreaterEqual(status['last_rtt_ms'], 0)
        self.assertEqual(status['reconnect_count'], 0)

        plugin = self.connect_plugin(route='x_u10')
        delta = hub.expect_frame(FRAME_ROUTE_DELTA)
        self.assertEqual(len(delta['upserted']), 1)
        self.assertEqual(delta['upserted'][0]['route'], 'x_u10')
        self.assertTrue(delta['upserted'][0]['appAlive'])
        self.assertEqual(delta['removed'], [])

        # Push plugin telemetry update (foreground + token expiry)
        plugin.send({
            'type': 'telemetry',
            'route': 'x_u10',
            'foreground': True,
            'cached_token_expires_at': 1899999999,
        })
        delta2 = hub.expect_frame(FRAME_ROUTE_DELTA)
        self.assertTrue(delta2['upserted'][0]['foreground'])
        self.assertEqual(delta2['upserted'][0]['cachedTokenExpiresAt'], 1899999999)

        # Send 2 concurrent RPCs and reply out-of-order
        hub.send_frame(FRAME_RPC_REQUEST, {
            'reqId': 101,
            'route': 'x_u10',
            'method': 'sign',
            'deadlineMs': 5000,
            'params': {'path': '/a'},
        })
        hub.send_frame(FRAME_RPC_REQUEST, {
            'reqId': 102,
            'route': 'x_u10',
            'method': 'sign',
            'deadlineMs': 5000,
            'params': {'path': '/b'},
        })
        fwd1 = plugin.recv()
        fwd2 = plugin.recv()
        self.assertEqual(fwd1['params']['path'], '/a')
        self.assertEqual(fwd2['params']['path'], '/b')

        # Reply to 102 first, then 101
        plugin.send({'type': 'response', 'id': fwd2['id'], 'route': 'x_u10', 'ok': True, 'result': {'sig': 'b'}})
        plugin.send({'type': 'response', 'id': fwd1['id'], 'route': 'x_u10', 'ok': True, 'result': {'sig': 'a'}})
        r1 = hub.expect_frame(FRAME_RPC_RESPONSE)
        r2 = hub.expect_frame(FRAME_RPC_RESPONSE)
        self.assertEqual(r1['reqId'], 102)
        self.assertEqual(r1['result'], {'sig': 'b'})
        self.assertEqual(r2['reqId'], 101)
        self.assertEqual(r2['result'], {'sig': 'a'})

        # Test RpcCancel aborts in-flight plugin request and drops late reply
        hub.send_frame(FRAME_RPC_REQUEST, {
            'reqId': 103,
            'route': 'x_u10',
            'method': 'token',
            'deadlineMs': 10000,
            'params': {},
        })
        fwd3 = plugin.recv()
        hub.send_frame(FRAME_RPC_CANCEL, {'reqId': 103, 'reason': 'caller_timeout'})
        cancel_msg = plugin.recv()
        self.assertEqual(cancel_msg, {'type': 'cancel', 'id': fwd3['id'], 'route': 'x_u10'})
        # Late plugin reply for cancelled job must be dropped silently
        plugin.send({'type': 'response', 'id': fwd3['id'], 'route': 'x_u10', 'ok': True, 'result': {'stale': True}})

        # Test v2 method == "wake" short-circuits when online & foreground=False
        hub.send_frame(FRAME_RPC_REQUEST, {
            'reqId': 104,
            'route': 'x_u10',
            'method': 'wake',
            'deadlineMs': 5000,
            'params': {'foreground': False},
        })
        wake_resp = hub.expect_frame(FRAME_RPC_RESPONSE)
        self.assertEqual(wake_resp['reqId'], 104)
        self.assertTrue(wake_resp['ok'])
        self.assertEqual(wake_resp['result'], {'online': True, 'woken': False})

        # Test 1 MiB response_too_large bounded v2 error frame
        hub.send_frame(FRAME_RPC_REQUEST, {
            'reqId': 105,
            'route': 'x_u10',
            'method': 'big',
            'deadlineMs': 5000,
            'params': {},
        })
        fwd_big = plugin.recv()
        base = {'type': 'response', 'id': fwd_big['id'], 'route': 'x_u10', 'ok': True, 'result': ''}
        pad = 1048576 - len(json.dumps(base, separators=(',', ':')).encode())
        base['result'] = 'z' * pad
        plugin.send(base)
        big_resp = hub.expect_frame(FRAME_RPC_RESPONSE)
        self.assertEqual(big_resp['reqId'], 105)
        self.assertFalse(big_resp['ok'])
        self.assertEqual(big_resp['errorCode'], 'response_too_large')
        self.assertFalse(big_resp['retryable'])

        # Plugin disconnect emits RouteDelta removed
        plugin.close()
        removed_delta = hub.expect_frame(FRAME_ROUTE_DELTA)
        self.assertEqual(removed_delta['removed'], ['x_u10'])

    def test_mixed_v1_default_and_v2_plugin_upstream(self):
        v1_listener = socket.socket()
        v1_listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        v1_listener.bind(('127.0.0.1', 0))
        v1_listener.listen(8)
        v1_listener.settimeout(5)
        self.addCleanup(v1_listener.close)

        v2_listener = socket.socket()
        v2_listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        v2_listener.bind(('127.0.0.1', 0))
        v2_listener.listen(8)
        v2_listener.settimeout(5)
        self.addCleanup(v2_listener.close)

        # Configure default hub as v1
        resp = self.ctl('configure', hub_host='127.0.0.1', hub_port=v1_listener.getsockname()[1],
                        device_id='dev-v1', device_token='tok-v1', hub_protocol='v1')
        self.assertTrue(resp['ok'])
        v1_hub = JsonlPeer(v1_listener.accept()[0])
        self.peers.append(v1_hub)
        self.assertEqual(v1_hub.recv()['type'], 'hello')

        # Install plugin router.json pointing to v2 upstream
        folder = self.state / 'plugins' / 'ghostv2'
        folder.mkdir(parents=True, exist_ok=True)
        (folder / 'manifest.json').write_text(json.dumps({
            'schemaVersion': 1, 'id': 'ghostv2', 'name': 'GhostV2', 'version': '1',
            'kind': 'lua', 'entry': 'main.lua', 'files': ['main.lua', 'router.json'],
            'router': 'router.json',
        }))
        (folder / 'router.json').write_text(json.dumps({
            'schemaVersion': 1,
            'services': ['x_v2'],
            'upstreams': [{
                'id': 'hub',
                'host': '127.0.0.1',
                'port': v2_listener.getsockname()[1],
                'device_id': 'dev-v2',
                'device_token': 'tok-v2',
                'protocol': 'v2',
            }],
            'default_upstream': 'hub',
        }))
        reload_resp = self.ctl('reload_plugins')
        self.assertTrue(reload_resp['ok'], reload_resp)
        v2_hub = V2Peer(v2_listener.accept()[0])
        self.peers.append(v2_hub)
        ft, hello = v2_hub.recv_frame()
        self.assertEqual(ft, FRAME_HELLO)
        self.assertEqual(hello['deviceId'], 'dev-v2')
        v2_hub.send_frame(FRAME_HELLO_ACK, {'ok': True, 'heartbeatIntervalSecs': 15, 'maxInflight': 32})
        ft, hb = v2_hub.recv_frame()
        self.assertEqual(ft, FRAME_HEARTBEAT)

        # Verify status shows both protocols cleanly
        status = self.ctl('status')['status']
        by_id = {u['id']: u for u in status['upstreams']}
        self.assertEqual(by_id['default']['protocol'], 'v1')
        self.assertEqual(by_id['ghostv2/hub']['protocol'], 'v2')

    def test_v2_tls_handshake_and_rpc(self):
        if not shutil.which('openssl'):
            self.skipTest('openssl binary not available')
        cert_path = self.state / 'hub.crt'
        key_path = self.state / 'hub.key'
        subprocess.run(
            [
                'openssl', 'req', '-x509', '-newkey', 'ec',
                '-pkeyopt', 'ec_paramgen_curve:prime256v1',
                '-keyout', str(key_path), '-out', str(cert_path),
                '-days', '1', '-nodes', '-subj', '/CN=localhost',
                '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1',
            ],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(certfile=str(cert_path), keyfile=str(key_path))
        raw_listener = socket.socket()
        raw_listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        raw_listener.bind(('127.0.0.1', 0))
        raw_listener.listen(8)
        raw_listener.settimeout(5)
        self.addCleanup(raw_listener.close)

        resp = self.ctl(
            'configure',
            hub_host='127.0.0.1',
            hub_port=raw_listener.getsockname()[1],
            device_id='phone-v2-tls',
            device_token='tls-secret',
            hub_protocol='v2',
            hub_tls=True,
            hub_tls_ca_file=str(cert_path),
            hub_tls_sni='localhost',
            hub_tls_insecure=False,
        )
        self.assertTrue(resp['ok'], resp)
        conn, _ = raw_listener.accept()
        tls_conn = ctx.wrap_socket(conn, server_side=True)
        hub = V2Peer(tls_conn)
        self.peers.append(hub)
        ft, hello = hub.recv_frame()
        self.assertEqual(ft, FRAME_HELLO)
        self.assertEqual(hello['deviceId'], 'phone-v2-tls')
        hub.send_frame(FRAME_HELLO_ACK, {'ok': True, 'heartbeatIntervalSecs': 15, 'maxInflight': 16})
        ft, hb = hub.recv_frame()
        self.assertEqual(ft, FRAME_HEARTBEAT)
        hub.send_frame(FRAME_HEARTBEAT_ACK, {'echoSentAtMs': hb['sentAtMs'], 'serverNowMs': int(time.time() * 1000)})

        status = self.ctl('status')['status']
        self.assertTrue(status['hub_connected'])
        self.assertTrue(status['hub_tls'])
        self.assertTrue(status.get('hub_tls_version', '').startswith('TLS'), status)

        plugin = self.connect_plugin(route='tls_route')
        delta = hub.expect_frame(FRAME_ROUTE_DELTA)
        self.assertEqual(delta['upserted'][0]['route'], 'tls_route')
        hub.send_frame(FRAME_RPC_REQUEST, {
            'reqId': 77, 'route': 'tls_route', 'method': 'ping', 'deadlineMs': 5000, 'params': {'v': 1},
        })
        fwd = plugin.recv()
        plugin.send({'type': 'response', 'id': fwd['id'], 'route': 'tls_route', 'ok': True, 'result': {'pong': True}})
        reply = hub.expect_frame(FRAME_RPC_RESPONSE)
        self.assertEqual(reply['reqId'], 77)
        self.assertTrue(reply['ok'])
        self.assertEqual(reply['result'], {'pong': True})


if __name__ == '__main__':
    unittest.main(verbosity=2)
