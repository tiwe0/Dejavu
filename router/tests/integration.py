#!/usr/bin/env python3
"""Isolated real-process protocol tests; no Android apps or real Hub required."""
import json
import os
from pathlib import Path
import socket
import stat
import subprocess
import tempfile
import time
import unittest

BINARY = Path(os.environ.get('ROUTER_BIN', 'out/router-host/dejavuerouter')).resolve()


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class Peer:
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
        self.last_line_length = len(line)
        return json.loads(line)

    def close(self):
        self.sock.close()


class RouterCase(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='dvr-', dir='/tmp')
        self.state = Path(self.directory.name)
        self.config = self.state / 'config.json'
        self.config.write_text(json.dumps({'listen_tcp': False, 'listen_port': free_port()}))
        self.log = open(self.state / 'test.log', 'wb')
        self.peers = []
        self.process = None
        self.start()

    def start(self):
        self.process = subprocess.Popen([str(BINARY), str(self.config)], stdout=self.log, stderr=self.log)
        metadata = self.state / 'control.json'
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.fail('daemon exited: ' + (self.state / 'test.log').read_text())
            if metadata.exists():
                self.metadata = json.loads(metadata.read_text())
                return
            time.sleep(.01)
        self.fail('daemon did not publish control metadata')

    def stop(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3)
                self.fail('daemon ignored termination')
        self.assertEqual(self.process.returncode, 0)
        self.process = None

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
        returncode = self.process.returncode if self.process else 0
        self.log.close()
        diagnostics = (self.state / 'test.log').read_text()
        self.directory.cleanup()
        self.assertEqual(returncode, 0, diagnostics)

    def ctl(self, op='status', **fields):
        sock = socket.socket(socket.AF_UNIX)
        sock.connect(self.metadata['name'])
        peer = Peer(sock)
        try:
            peer.send({'token': self.metadata['token'], 'op': op, **fields})
            return peer.recv()
        finally:
            peer.close()

    def status(self):
        response = self.ctl()
        self.assertTrue(response['ok'], response)
        return response['status']

    def plugin(self, route='demo', app='org.example.demo'):
        sock = socket.socket(socket.AF_UNIX)
        sock.connect(str(self.state / 'plugins.sock'))
        peer = Peer(sock)
        self.peers.append(peer)
        peer.send({'type': 'hello', 'app': app, 'routes': [route]})
        self.assertEqual(peer.recv()['type'], 'hello_ok')
        return peer

    def hub(self, host="127.0.0.1"):
        listener = socket.socket()
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        listener.settimeout(5)
        self.addCleanup(listener.close)
        response = self.ctl('configure', hub_host=host, hub_port=listener.getsockname()[1],
                            device_id='unit-device', device_token='test-credential-never-log')
        self.assertTrue(response['ok'])
        peer = Peer(listener.accept()[0])
        self.peers.append(peer)
        hello = peer.recv()
        self.assertEqual(hello['type'], 'hello')
        self.assertEqual(hello['token'], 'test-credential-never-log')
        return peer, listener

    def test_legacy_tcp_plugin_protocol(self):
        self.stop()
        config = json.loads(self.config.read_text())
        config['listen_tcp'] = True
        config['listen_port'] = free_port()
        self.config.write_text(json.dumps(config))
        self.start()
        plugin = Peer(socket.create_connection(('127.0.0.1', config['listen_port'])))
        self.peers.append(plugin)
        plugin.send({'type': 'hello', 'app': 'org.example.legacy', 'routes': ['legacy-route']})
        self.assertEqual(plugin.recv()['type'], 'hello_ok')
        plugin.send({'type': 'ping'})
        self.assertEqual(plugin.recv()['type'], 'pong')
        status = self.status()
        self.assertTrue(status['listen_tcp'])
        self.assertTrue(next(row for row in status['routes'] if row['route'] == 'legacy-route')['online'])

    def test_authentication_metadata_and_offline(self):
        self.assertEqual(self.metadata['transport'], 'unix')
        self.assertEqual(self.metadata['namespace'], 'filesystem')
        self.assertEqual(len(self.metadata['token']), 64)
        self.assertEqual(stat.S_IMODE((self.state / 'control.json').stat().st_mode), 0o600)
        self.assertEqual(self.ctl(token='wrong'), {'ok': False, 'error': 'unauthorized'})
        snapshot = self.status()
        self.assertFalse(snapshot['hub_connected'])
        self.assertFalse(snapshot['listen_tcp'])
        self.assertEqual(snapshot['routes'], [])
        self.assertEqual(self.ctl('arbitrary-command')['error'], 'invalid_operation')
        with socket.socket() as sock:
            self.assertNotEqual(sock.connect_ex(('127.0.0.1', snapshot['listen_port'])), 0)

    def test_configuration_validation_and_persistence(self):
        response = self.ctl('configure', hub_host='', hub_port=39010, device_id='unit-device', device_token='secret-value')
        self.assertTrue(response['ok'])
        self.assertTrue(response['status']['token_configured'])
        self.assertNotIn('secret-value', json.dumps(response))
        before = self.config.read_bytes()
        for fields in ({'hub_port': 3.5}, {'hub_port': 65536}, {'hub_host': 'bad\nvalue'}, {'device_id': ''}):
            config = {'hub_host': '', 'hub_port': 39010, 'device_id': 'unit-device', **fields}
            self.assertFalse(self.ctl('configure', **config)['ok'])
            self.assertEqual(self.config.read_bytes(), before)
        self.assertTrue(self.ctl('configure', hub_host='', hub_port=39011, device_id='changed')['ok'])
        self.assertEqual(json.loads(self.config.read_text())['device_token'], 'secret-value')
        self.assertEqual(stat.S_IMODE(self.config.stat().st_mode), 0o600)
        old_token = self.metadata['token']
        self.stop()
        self.assertFalse((self.state / 'control.json').exists())
        self.start()
        self.assertNotEqual(old_token, self.metadata['token'])
        self.assertEqual(self.status()['device_id'], 'changed')
        self.assertTrue(self.status()['token_configured'])
        self.assertNotIn('secret-value', (self.state / 'test.log').read_text())

    def test_route_rules_are_atomic_and_separate_from_live_routes(self):
        plugin = self.plugin()
        fields = {'route': 'demo', 'user_id': 0, 'package': 'org.example.demo', 'activity': '.Main',
                  'plugin_path': '/data/adb/dejavu/plugins/demo/libdemo.so'}
        self.assertTrue(self.ctl('route_put', **fields)['ok'])
        route = self.status()['routes'][0]
        self.assertTrue(route['online'])
        self.assertTrue(route['configured'])
        self.assertEqual(route['app'], 'org.example.demo')
        target_path = self.state / 'wake_targets.json'
        before = target_path.read_bytes()
        for changes in ({'user_id': -1}, {'user_id': 1.5}, {'package': '-evil'}, {'activity': 'x/y'}, {'plugin_path': 'relative'}):
            self.assertFalse(self.ctl('route_put', **{**fields, **changes})['ok'])
            self.assertEqual(target_path.read_bytes(), before)
        self.assertEqual(stat.S_IMODE(target_path.stat().st_mode), 0o600)
        self.assertTrue(self.ctl('route_delete', route='demo')['ok'])
        route = self.status()['routes'][0]
        self.assertFalse(route['configured'])
        self.assertTrue(route['online'])
        plugin.close()
        for _ in range(50):
            if not self.status()['routes']:
                break
            time.sleep(.01)
        self.assertEqual(self.status()['routes'], [])
        self.assertTrue(self.ctl('route_put', **fields)['ok'])
        self.stop()
        self.start()
        self.assertEqual(self.status()['routes'][0]['package'], fields['package'])
        self.assertFalse(self.status()['routes'][0]['online'])

    def test_forwarding_deadlines_duplicates_and_route_mismatch(self):
        hub, _ = self.hub()
        plugin = self.plugin()
        self.assertEqual(hub.recv()['action'], 'register')
        hub.send({'type': 'request', 'method': 'inspect', 'id': '1', 'route': 'demo', 'params': {'body': 'PRIVATE_TEST_BODY'}})
        forwarded = plugin.recv()
        self.assertEqual(forwarded['params'], {'body': 'PRIVATE_TEST_BODY'})
        hub.send({'type': 'request', 'method': 'inspect', 'id': '1', 'route': 'demo'})
        self.assertEqual(hub.recv()['error']['code'], 'duplicate_id')
        plugin.send({'type': 'response', 'id': forwarded['id'], 'route': 'demo', 'ok': True, 'result': {'answer': 42}})
        self.assertEqual(hub.recv(), {'type': 'response', 'id': '1', 'route': 'demo', 'ok': True, 'result': {'answer': 42}})
        hub.send({'type': 'request', 'method': 'inspect', 'id': '2', 'route': 'demo', 'deadline_ms': 100})
        timed = plugin.recv()
        self.assertEqual(hub.recv()['error']['code'], 'plugin_timeout')
        self.assertEqual(plugin.recv(), {'type': 'cancel', 'id': timed['id'], 'route': 'demo'})
        hub.send({'type': 'request', 'method': 'inspect', 'id': '3', 'route': 'demo'})
        mismatched = plugin.recv()
        plugin.send({'type': 'response', 'id': mismatched['id'], 'route': 'different', 'ok': True})
        self.assertEqual(hub.recv()['error']['code'], 'route_mismatch')
        self.assertEqual(self.status()['pending_jobs'], 0)
        self.assertNotIn('PRIVATE_TEST_BODY', (self.state / 'test.log').read_text())

    def test_deadline_budget_is_authoritative_and_cancellation_is_route_scoped(self):
        hub, _ = self.hub()
        plugin = self.plugin()
        hub.recv()
        for index, (value, effective) in enumerate(((None, 60000), (0, 60000), (-1, 60000),
                                                      (1000, 1000), (1e308, 86400000))):
            request = {'type': 'request', 'method': 'inspect', 'id': str(index), 'route': 'demo',
                       'deadline_monotonic_ms': 1}
            if value is not None:
                request['deadline_ms'] = value
            before = time.clock_gettime_ns(time.CLOCK_MONOTONIC) // 1000000
            hub.send(request)
            forwarded = plugin.recv()
            after = time.clock_gettime_ns(time.CLOCK_MONOTONIC) // 1000000
            self.assertEqual(forwarded['deadline_ms'], effective)
            self.assertGreaterEqual(forwarded['deadline_monotonic_ms'], before + effective)
            self.assertLessEqual(forwarded['deadline_monotonic_ms'], after + effective)
            # A reused ID on another route cannot cancel an admitted request.
            hub.send({'type': 'cancel', 'id': str(index), 'route': 'wrong-route'})
            hub.send({'type': 'ping'})
            self.assertEqual(hub.recv()['type'], 'pong')
            self.assertEqual(self.status()['pending_jobs'], 1)
            hub.send({'type': 'cancel', 'id': str(index), 'route': 'demo'})
            self.assertEqual(plugin.recv(), {'type': 'cancel', 'id': forwarded['id'], 'route': 'demo'})
            self.assertEqual(self.status()['pending_jobs'], 0)
        plugin.send({'type': 'response', 'id': forwarded['id'], 'route': 'demo', 'ok': True})
        hub.send({'type': 'ping'})
        self.assertEqual(hub.recv()['type'], 'pong')

    def test_hub_disconnect_cancels_pending_plugin_work(self):
        hub, listener = self.hub()
        plugin = self.plugin()
        hub.recv()
        hub.send({'type': 'request', 'method': 'inspect', 'id': 'abandoned', 'route': 'demo'})
        abandoned = plugin.recv()
        hub.close()
        self.assertEqual(plugin.recv(), {'type': 'cancel', 'id': abandoned['id'], 'route': 'demo'})
        reconnected = Peer(listener.accept()[0])
        self.peers.append(reconnected)
        self.assertEqual(reconnected.recv()['type'], 'hello')
        self.assertEqual(reconnected.recv()['action'], 'register')
        self.assertEqual(self.status()['pending_jobs'], 0)
        plugin.send({'type': 'response', 'id': abandoned['id'], 'route': 'demo', 'ok': True})
        reconnected.send({'type': 'ping'})
        self.assertEqual(reconnected.recv()['type'], 'pong')

    def assert_reused_id_ignores_old_response(self, retirement):
        hub, listener = self.hub()
        plugin = self.plugin()
        hub.recv()
        request = {'type': 'request', 'id': 'reused', 'route': 'demo', 'method': 'inspect'}
        hub.send({**request, 'deadline_ms': 100 if retirement == 'timeout' else 60000})
        old = plugin.recv()
        if retirement == 'timeout':
            self.assertEqual(hub.recv()['error']['code'], 'plugin_timeout')
        elif retirement == 'cancel':
            hub.send({'type': 'cancel', 'id': request['id'], 'route': 'demo'})
        else:
            hub.close()
        self.assertEqual(plugin.recv(), {'type': 'cancel', 'id': old['id'], 'route': 'demo'})
        if retirement == 'reconnect':
            hub = Peer(listener.accept()[0])
            self.peers.append(hub)
            self.assertEqual(hub.recv()['type'], 'hello')
            self.assertEqual(hub.recv()['action'], 'register')
        hub.send(request)
        new = plugin.recv()
        self.assertNotEqual(old['id'], new['id'])
        plugin.send({'type': 'response', 'id': old['id'], 'route': 'demo', 'ok': True, 'result': 'old'})
        # Ping on the same plugin stream proves its stale response was processed.
        plugin.send({'type': 'ping'})
        self.assertEqual(plugin.recv()['type'], 'pong')
        self.assertEqual(self.status()['pending_jobs'], 1)
        plugin.send({'type': 'response', 'id': new['id'], 'route': 'demo', 'ok': True, 'result': 'new'})
        self.assertEqual(hub.recv(), {'type': 'response', 'id': 'reused', 'route': 'demo',
                                      'ok': True, 'result': 'new'})
        self.assertEqual(self.status()['pending_jobs'], 0)

    def test_id_reuse_after_timeout_ignores_stale_response(self):
        self.assert_reused_id_ignores_old_response('timeout')

    def test_id_reuse_after_cancel_ignores_stale_response(self):
        self.assert_reused_id_ignores_old_response('cancel')

    def test_id_reuse_after_hub_reconnect_ignores_stale_response(self):
        self.assert_reused_id_ignores_old_response('reconnect')

    def test_wire_ids_are_separate_and_responses_are_plugin_scoped(self):
        hub, _ = self.hub()
        first = self.plugin()
        hub.recv()
        other = self.plugin('other')
        hub.recv()
        hub.send({'type': 'request', 'id': 'external', 'route': 'demo', 'method': 'inspect'})
        wire = first.recv()['id']
        # Hub IDs may equal a previous wire ID without becoming aliases.
        hub.send({'type': 'request', 'id': wire, 'route': 'other', 'method': 'inspect'})
        other_wire = other.recv()['id']
        other.send({'type': 'response', 'id': wire, 'route': 'demo', 'ok': True, 'result': 'wrong-plugin'})
        other.send({'type': 'ping'})
        self.assertEqual(other.recv()['type'], 'pong')
        self.assertEqual(self.status()['pending_jobs'], 2)
        hub.send({'type': 'cancel', 'id': wire, 'route': 'other'})
        self.assertEqual(other.recv(), {'type': 'cancel', 'id': other_wire, 'route': 'other'})
        first.send({'type': 'response', 'id': wire, 'route': 'demo', 'ok': True, 'result': 'right'})
        self.assertEqual(hub.recv()['result'], 'right')
        self.assertEqual(self.status()['pending_jobs'], 0)

    def test_invalid_jobs_do_not_disconnect_plugin_or_cancel_pending_work(self):
        hub, _ = self.hub()
        plugin = self.plugin()
        hub.recv()
        request = {'type': 'request', 'id': 'pending', 'route': 'demo', 'method': 'inspect'}
        hub.send(request)
        pending = plugin.recv()
        invalid = [{'method': value} for value in (None, 42, '', 'a b', 'a/b', 'a\nb', 'a\x00b', 'é', 'x' * 129)]
        invalid += [{'id': value} for value in ('', 'x' * 129, 'bad\rid', 'bad\x7fid', 'bad\x00id', 42)]
        invalid += [{'route': value} for value in ('', 'x' * 129, 'bad\nroute', 'bad\x00route', 42)]
        invalid_requests = [{**request, **fields} for fields in invalid]
        invalid_requests.append({key: value for key, value in request.items() if key != 'method'})
        for bad in invalid_requests:
            with self.subTest(request=bad):
                hub.send(bad)
                self.assertEqual(hub.recv()['error']['code'], 'invalid_job')
                self.assertEqual(self.status()['pending_jobs'], 1)
        plugin.send({'type': 'response', 'id': pending['id'], 'route': 'demo', 'ok': True, 'result': 'alive'})
        self.assertEqual(hub.recv()['result'], 'alive')
        # Params are opaque JSON; absence, null and scalar values remain valid.
        for index, params in enumerate((None, 42, 'text', [], {'x': True})):
            hub.send({**request, 'id': str(index), 'method': 'A_1.-', 'params': params})
            forwarded = plugin.recv()
            self.assertEqual(forwarded['params'], params)
            plugin.send({'type': 'response', 'id': forwarded['id'], 'route': 'demo', 'ok': True})
            self.assertEqual(hub.recv()['id'], str(index))
        self.assertEqual(self.status()['plugin_count'], 1)

    def test_forwarded_frame_limit_includes_injected_fields(self):
        hub, _ = self.hub()
        plugin = self.plugin()
        hub.recv()
        request = {'type': 'request', 'id': 'large', 'route': 'demo', 'method': 'inspect', 'params': ''}
        size = len(json.dumps(request, separators=(',', ':')).encode())
        request['params'] = 'x' * (1024 * 1024 - size)
        self.assertEqual(len(json.dumps(request, separators=(',', ':')).encode()), 1024 * 1024)
        hub.send(request)
        self.assertEqual(hub.recv()['error']['code'], 'invalid_job')
        self.assertEqual(self.status()['pending_jobs'], 0)
        plugin.send({'type': 'ping'})
        self.assertEqual(plugin.recv()['type'], 'pong')
        hub.send({'type': 'request', 'id': 'small', 'route': 'demo', 'method': 'inspect'})
        forwarded = plugin.recv()
        plugin.send({'type': 'response', 'id': forwarded['id'], 'route': 'demo', 'ok': True})
        self.assertEqual(hub.recv()['id'], 'small')

    def test_response_frame_limit_applies_after_external_id_restoration(self):
        hub, _ = self.hub()
        plugin = self.plugin()
        hub.recv()
        external_id = 'h' * 128
        for ok, payload in ((True, ''), (False, ''), (False, {'padding': ''})):
            with self.subTest(ok=ok, payload_type=type(payload).__name__):
                hub.send({'type': 'request', 'id': external_id, 'route': 'demo', 'method': 'inspect'})
                forwarded = plugin.recv()
                hub.send({'type': 'request', 'id': 'other', 'route': 'demo', 'method': 'inspect'})
                other = plugin.recv()
                field = 'result' if ok else 'error'
                response = {'type': 'response', 'id': forwarded['id'], 'route': 'demo', 'ok': ok, field: payload}
                size = len(json.dumps(response, separators=(',', ':')).encode())
                padding = 'x' * (1024 * 1024 - size)
                response[field] = {'padding': padding} if isinstance(payload, dict) else padding
                self.assertEqual(len(json.dumps(response, separators=(',', ':')).encode()), 1024 * 1024)
                plugin.send(response)
                rejected = hub.recv()
                self.assertLessEqual(hub.last_line_length, 1024 * 1024)
                self.assertEqual(rejected['id'], external_id)
                self.assertEqual(rejected['route'], 'demo')
                self.assertFalse(rejected['ok'])
                self.assertEqual(rejected['error']['code'], 'response_too_large')
                self.assertEqual(self.status()['pending_jobs'], 1)
                plugin.send({'type': 'response', 'id': other['id'], 'route': 'demo', 'ok': True, 'result': 'alive'})
                self.assertEqual(hub.recv(), {'type': 'response', 'id': 'other', 'route': 'demo',
                                              'ok': True, 'result': 'alive'})
                self.assertEqual(self.status()['pending_jobs'], 0)
        self.assertTrue(self.status()['hub_connected'])
        self.assertEqual(self.status()['plugin_count'], 1)

    def test_plugin_hello_deadline_is_absolute_and_releases_slots(self):
        registered = self.plugin()
        pending = []
        started = time.monotonic()
        for _ in range(63):
            sock = socket.socket(socket.AF_UNIX)
            sock.connect(str(self.state / 'plugins.sock'))
            sock.settimeout(.001)
            pending.append(sock)
            self.addCleanup(sock.close)
        alive = list(pending)
        while alive and time.monotonic() - started < 6.5:
            for sock in list(alive):
                try:
                    sock.sendall(b' ')  # Regular progress must not extend hello admission.
                    try:
                        chunk = sock.recv(1)
                        self.assertEqual(chunk, b'')
                        alive.remove(sock)
                    except socket.timeout:
                        pass
                except (BrokenPipeError, ConnectionResetError):
                    alive.remove(sock)
            time.sleep(.05)
        self.assertEqual(alive, [], 'trickling peers retained plugin slots beyond hello deadline')
        self.assertLess(time.monotonic() - started, 7)
        registered.send({'type': 'ping'})
        self.assertEqual(registered.recv()['type'], 'pong')
        self.plugin('after-deadline')
        self.assertEqual(self.status()['plugin_count'], 2)

    def test_invalid_and_repeated_hello_are_rejected_and_closed(self):
        for hello in ({'type': 'hello', 'app': '', 'routes': ['bad']},
                      {'type': 'hello', 'app': 'bad\napp', 'routes': ['bad']},
                      {'type': 'hello', 'app': 'org.example.demo', 'routes': ['bad\nroute']},
                      {'type': 'hello', 'app': 'org.example.demo', 'routes': []},
                      {'type': 'hello', 'app': 'org.example.demo', 'routes': ['bad', 'bad']},
                      {'type': 'ping'}):
            with self.subTest(hello=hello):
                sock = socket.socket(socket.AF_UNIX)
                sock.connect(str(self.state / 'plugins.sock'))
                peer = Peer(sock)
                try:
                    peer.send(hello)
                    rejected = peer.recv()
                    self.assertEqual(rejected['type'], 'hello_error')
                    self.assertEqual(rejected['error']['code'], 'invalid_hello')
                    with self.assertRaises((EOFError, ConnectionResetError)):
                        peer.recv()
                finally:
                    peer.close()
                self.assertEqual(self.status()['plugin_count'], 0)
        registered = self.plugin()
        registered.send({'type': 'hello', 'app': 'org.example.demo', 'routes': ['replacement']})
        rejected = registered.recv()
        self.assertEqual(rejected['type'], 'hello_error')
        self.assertEqual(rejected['error']['code'], 'already_registered')
        with self.assertRaises((EOFError, ConnectionResetError)):
            registered.recv()
        self.assertEqual(self.status()['routes'], [])

    def test_omitted_transport_option_defaults_to_unix_only(self):
        self.stop()
        self.config.write_text(json.dumps({'listen_port': free_port()}))
        self.start()
        snapshot = self.status()
        self.assertFalse(snapshot['listen_tcp'])
        with socket.socket() as sock:
            self.assertNotEqual(sock.connect_ex(('127.0.0.1', snapshot['listen_port'])), 0)
        self.plugin()

    def test_plugin_disconnect_and_oversized_input_cleanup(self):
        hub, _ = self.hub()
        plugin = self.plugin()
        hub.recv()
        hub.send({'type': 'request', 'method': 'inspect', 'id': 'pending', 'route': 'demo'})
        plugin.recv()
        plugin.sock.sendall(b'x' * (2 * 1024 * 1024 + 1))
        error = hub.recv()
        self.assertEqual(error['error']['code'], 'plugin_disconnected')
        self.assertEqual(hub.recv()['action'], 'unregister')
        self.assertEqual(self.status()['plugin_count'], 0)
        self.assertEqual(self.status()['pending_jobs'], 0)
        self.assertEqual(self.status()['routes'], [])
        replacement = self.plugin()
        self.assertEqual(hub.recv()['action'], 'register')
        replacement.send({'type': 'ping'})
        self.assertEqual(replacement.recv()['type'], 'pong')

    def test_reconnect_drops_ambiguous_jobs_without_replay(self):
        hub, listener = self.hub()
        plugin = self.plugin()
        hub.recv()
        hub.send({'type': 'request', 'method': 'inspect', 'id': 'pending', 'route': 'demo'})
        pending = plugin.recv()
        self.assertTrue(self.ctl('reconnect')['ok'])
        self.assertEqual(plugin.recv(), {'type': 'cancel', 'id': pending['id'], 'route': 'demo'})
        reconnect = Peer(listener.accept()[0])
        self.peers.append(reconnect)
        self.assertEqual(reconnect.recv()['type'], 'hello')
        self.assertEqual(reconnect.recv()['action'], 'register')
        self.assertEqual(self.status()['pending_jobs'], 0)
        plugin.send({'type': 'response', 'id': pending['id'], 'route': 'demo', 'ok': True})
        reconnect.send({'type': 'ping'})
        self.assertEqual(reconnect.recv()['type'], 'pong')

    def test_input_limits_do_not_break_controls(self):
        sock = socket.socket(socket.AF_UNIX)
        sock.connect(self.metadata['name'])
        sock.sendall(b'x' * (32768 + 1))
        try:
            self.assertEqual(sock.recv(1), b'')
        except ConnectionResetError:
            pass
        finally:
            sock.close()
        self.assertFalse(self.ctl('status', token='x' * 64)['ok'])
        self.assertTrue(self.ctl()['ok'])

    def test_nul_strings_and_trailing_payload_are_rejected(self):
        for body in (
            {'token': self.metadata['token'], 'op': 'status\x00configure'},
            {'token': self.metadata['token'], 'op': 'route_put', 'route': 'demo\x00alias',
             'user_id': 0, 'package': 'org.example.demo', 'activity': '.Main'},
        ):
            sock = socket.socket(socket.AF_UNIX)
            sock.connect(self.metadata['name'])
            peer = Peer(sock)
            try:
                peer.send(body)
                self.assertEqual(peer.recv(), {'ok': False, 'error': 'invalid_request'})
            finally:
                peer.close()
        sock = socket.socket(socket.AF_UNIX)
        sock.connect(self.metadata['name'])
        peer = Peer(sock)
        try:
            payload = json.dumps({'token': self.metadata['token'], 'op': 'status'}).encode()
            sock.sendall(payload + b'{}\n')
            self.assertEqual(peer.recv(), {'ok': False, 'error': 'invalid_request'})
        finally:
            peer.close()
        self.assertEqual(self.status()['routes'], [])

    def test_hub_oversize_disconnect_reconnects(self):
        hub, listener = self.hub()
        hub.sock.sendall(b'x' * (2 * 1024 * 1024 + 1))
        connection = Peer(listener.accept()[0])
        self.peers.append(connection)
        self.assertEqual(connection.recv()['type'], 'hello')
        connection.send({'type': 'ping'})
        self.assertEqual(connection.recv()['type'], 'pong')

    def test_single_instance_and_startup_failure_clear_stale_metadata(self):
        second = subprocess.run([str(BINARY), str(self.config)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=3)
        self.assertNotEqual(second.returncode, 0)
        self.assertTrue(self.ctl()['ok'])
        self.stop()
        (self.state / 'control.json').write_text('{"stale":true}')
        self.config.write_text('{"listen_host":"0.0.0.0"}')
        failed = subprocess.run([str(BINARY), str(self.config)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=3)
        self.assertNotEqual(failed.returncode, 0)
        self.assertFalse((self.state / 'control.json').exists())

    def test_socket_names_roundtrip_and_rule_limit(self):
        self.stop()
        config = json.loads(self.config.read_text())
        config.update(control_socket='test.control', plugin_socket='test.plugins')
        self.config.write_text(json.dumps(config))
        targets = {str(i): {'user_id': 0, 'package': 'org.example.demo', 'activity': '.Main'} for i in range(256)}
        (self.state / 'wake_targets.json').write_text(json.dumps(targets))
        self.start()
        self.assertEqual(self.metadata['name'], str(self.state / 'test.control'))
        self.assertEqual(len(self.status()['routes']), 256)
        self.assertFalse(self.ctl('route_put', route='overflow', user_id=0, package='org.example.demo', activity='.Main')['ok'])
        self.assertTrue(self.ctl('configure', hub_host='', hub_port=39010, device_id='unit-device')['ok'])
        self.stop()
        self.start()
        self.assertEqual(self.metadata['name'], str(self.state / 'test.control'))

    def test_platform_dns_falls_back_to_ipv4_and_controls_stay_responsive(self):
        hub, _ = self.hub(host='localhost')
        hub.send({'type': 'ping'})
        self.assertEqual(hub.recv()['type'], 'pong')
        started = time.monotonic()
        self.assertTrue(self.ctl('configure', hub_host='invalid.dejavu-test.invalid', hub_port=39010,
                                 device_id='unit-device')['ok'])
        for _ in range(10):
            self.assertTrue(self.ctl()['ok'])
        self.assertLess(time.monotonic() - started, 2)
        self.assertTrue(self.ctl('configure', hub_host='', hub_port=39010, device_id='unit-device')['ok'])
        self.assertFalse(self.status()['hub_connected'])

    def test_duplicate_routes_are_not_registered(self):
        first = self.plugin()
        sock = socket.socket(socket.AF_UNIX)
        sock.connect(str(self.state / 'plugins.sock'))
        duplicate = Peer(sock)
        self.peers.append(duplicate)
        duplicate.send({'type': 'hello', 'app': 'other', 'routes': ['demo']})
        rejected = duplicate.recv()
        self.assertEqual(rejected['type'], 'hello_error')
        self.assertEqual(rejected['error']['code'], 'route_conflict')
        with self.assertRaises((EOFError, ConnectionResetError)):
            duplicate.recv()
        self.assertEqual(self.status()['plugin_count'], 1)
        self.assertEqual(self.status()['routes'][0]['app'], 'org.example.demo')
        first.send({'type': 'ping'})
        self.assertEqual(first.recv()['type'], 'pong')


if __name__ == '__main__':
    unittest.main(verbosity=2)
