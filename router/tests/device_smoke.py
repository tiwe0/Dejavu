#!/usr/bin/env python3
"""Isolated Android router smoke; never touches installed daemon/config or UI.

Usage: python3 router/tests/device_smoke.py --adb /path/to/adb --serial SERIAL --binary PATH
"""
import argparse
import json
import secrets
import re
import shlex
import socket
import subprocess
import tempfile
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', default='adb')
    parser.add_argument('--serial', required=True)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--instrumentation', help='Optional Manager instrumentation component')
    parser.add_argument('--manager-package', default='cafe.ivory.dejavu.debug')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+)+', args.manager_package):
        parser.error('Invalid Manager package')
    adb = [args.adb, '-s', args.serial]
    device_dir = '/data/local/tmp/dejavu-router-smoke-' + secrets.token_hex(6)
    device_bin = device_dir + '/dejavuerouter'
    pid = None
    fixture = None
    forwards = []
    reverse_port = None
    sockets = []

    def run(*command):
        return subprocess.check_output(adb + list(command), text=True, timeout=30).strip()

    def root(command):
        return run('shell', 'su -M -c ' + shlex.quote(command))

    def send(sock, message):
        sock.sendall(json.dumps(message, separators=(',', ':')).encode() + b'\n')

    def receive(stream):
        line = stream.readline(1048578)
        if not line.endswith(b'\n') or len(line) - 1 > 1048576:
            raise AssertionError('Missing or oversized protocol response')
        return json.loads(line)

    def expected(stream, kind, job_id=None):
        for _ in range(1024):
            message = receive(stream)
            if message.get('type') == kind and (job_id is None or message.get('id') == job_id):
                return message
        raise AssertionError('Expected protocol message was not received')

    try:
        assert root('id -u') == '0', 'Root access required'
        with socket.socket() as hub:
            hub.bind(('127.0.0.1', 0))
            hub.listen(8)
            hub.settimeout(10)
            # Independent synthetic endpoints: existing router listens on 39111/39112.
            base = 41000 + secrets.randbelow(1000) * 3
            hub_port, plugin_port = base, base + 1
            run('reverse', f'tcp:{hub_port}', f'tcp:{hub.getsockname()[1]}')
            reverse_port = hub_port
            suffix = device_dir.rsplit('-', 1)[-1]
            plugin_socket = 'dejavu.router.test.plugins.' + suffix
            control_socket = 'dejavu.router.test.control.' + suffix
            run('shell', 'mkdir -p ' + shlex.quote(device_dir))
            run('push', str(args.binary.resolve()), device_bin)
            config = dict(hub_host='localhost', hub_port=hub_port,
                          device_id='isolated-device-test', device_token='synthetic-test-token',
                          listen_host='127.0.0.1', listen_port=plugin_port,
                          control_socket=control_socket, plugin_socket=plugin_socket)
            with tempfile.TemporaryDirectory(prefix='dejavu-router-device-') as tmp:
                config_path = Path(tmp) / 'config.json'
                config_path.write_text(json.dumps(config))
                run('push', str(config_path), device_dir + '/config.json')
            root(f'chown 0:0 {shlex.quote(device_dir)} {shlex.quote(device_bin)} {shlex.quote(device_dir + "/config.json")}; '
                 f'chmod 700 {shlex.quote(device_dir)} {shlex.quote(device_bin)}; '
                 f'chmod 600 {shlex.quote(device_dir + "/config.json")}')
            pid = int(root(f'setsid {shlex.quote(device_bin)} {shlex.quote(device_dir + "/config.json")} '
                           '</dev/null >/dev/null 2>&1 & echo $!'))
            metadata = None
            for _ in range(30):
                try:
                    metadata = json.loads(root('cat ' + shlex.quote(device_dir + '/control.json') + ' 2>/dev/null'))
                    break
                except (subprocess.CalledProcessError, ValueError):
                    time.sleep(.1)
            if not metadata:
                diagnostic = root('timeout 2 ' + shlex.quote(device_bin) + ' ' + shlex.quote(device_dir + '/config.json') + ' 2>&1 || true')
                raise AssertionError('Isolated daemon did not publish metadata: ' + diagnostic[:500])
            assert metadata['transport'] == 'unix'
            assert metadata['namespace'] == 'abstract' and metadata['name'] == control_socket
            for name in (plugin_socket, control_socket):
                local = int(run('forward', 'tcp:0', 'localabstract:' + name))
                forwards.append(local)
            assert root('stat -c %a ' + shlex.quote(device_dir + '/control.json')) == '600'

            def control(op, **fields):
                with socket.create_connection(('127.0.0.1', forwards[1]), timeout=5) as client:
                    send(client, dict(token=metadata['token'], op=op, **fields))
                    result = receive(client.makefile('rb'))
                    assert result.get('ok') is True, result.get('error', 'Control failed')
                    # Authentication and Hub credentials must not leak into status.
                    serialized = json.dumps(result)
                    assert metadata['token'] not in serialized
                    assert 'synthetic-test-token' not in serialized
                    return result['status']

            hub_client, _ = hub.accept()
            sockets.append(hub_client)
            hub_client.settimeout(5)
            hub_stream = hub_client.makefile('rb')
            assert expected(hub_stream, 'hello')['device_id'] == 'isolated-device-test'
            rule = dict(route='synthetic-route', user_id=0, package='com.example.routertest',
                        activity='.MainActivity', plugin_path='')
            control('route_put', **rule)
            plugin = socket.create_connection(('127.0.0.1', forwards[0]), timeout=5)
            sockets.append(plugin)
            plugin_stream = plugin.makefile('rb')
            send(plugin, dict(type='hello', app='com.example.routertest', version=1, routes=[rule['route']]))
            expected(plugin_stream, 'hello_ok')
            expected(hub_stream, 'route')
            status = control('status')
            assert status['hub_connected'] and status['plugin_count'] == 1
            route = next(row for row in status['routes'] if row['route'] == rule['route'])
            assert route['online'] and route['configured']
            request = dict(type='request', id='synthetic-job', route=rule['route'],
                           method='echo', params={'value': 'memory-only'}, deadline_ms=2000)
            send(hub_client, request)
            forwarded = expected(plugin_stream, 'request')
            assert forwarded['params'] == request['params']
            send(plugin, dict(type='response', id=forwarded['id'], route=rule['route'],
                             ok=True, result={'value': 'memory-only'}))
            response = expected(hub_stream, 'response', request['id'])
            assert response['ok'] and response['result']['value'] == 'memory-only'
            request.update(id='synthetic-timeout', deadline_ms=100)
            send(hub_client, request)
            retired = expected(plugin_stream, 'request')
            response = expected(hub_stream, 'response', request['id'])
            assert response['ok'] is False and response['error']['code'] == 'plugin_timeout', response
            assert receive(plugin_stream) == dict(type='cancel', id=retired['id'], route=rule['route'])

            # Reusing the external ID must never let the retired wire ID complete new work.
            request.update(deadline_ms=5000)
            send(hub_client, request)
            replacement = expected(plugin_stream, 'request')
            assert replacement['id'] != retired['id']
            send(hub_client, dict(type='request', id='invalid-method', route=rule['route'], method='bad method'))
            invalid = receive(hub_stream)
            assert invalid['id'] == 'invalid-method' and invalid['ok'] is False, invalid
            assert invalid['error']['code'] == 'invalid_job', invalid
            send(plugin, dict(type='response', id=retired['id'], route=rule['route'], ok=True, result='old'))
            # Same-stream pong confirms the stale response was handled before checking pending work.
            send(plugin, dict(type='ping'))
            assert receive(plugin_stream)['type'] == 'pong'
            status = control('status')
            assert status['pending_jobs'] == 1 and status['plugin_count'] == 1 and status['hub_connected'], status
            send(plugin, dict(type='response', id=replacement['id'], route=rule['route'], ok=True, result='new'))
            response = receive(hub_stream)
            assert response == dict(type='response', id=request['id'], route=rule['route'], ok=True, result='new'), response
            assert control('status')['pending_jobs'] == 0

            # Restoring the longest external ID can grow a legal plugin frame past the Hub limit.
            long_id = 'h' * 128
            send(hub_client, dict(type='request', id=long_id, route=rule['route'], method='echo', deadline_ms=5000))
            large = expected(plugin_stream, 'request')
            large_response = dict(type='response', id=large['id'], route=rule['route'], ok=True, result='')
            overhead = len(json.dumps(large_response, separators=(',', ':')).encode())
            large_response['result'] = 'x' * (1048576 - overhead)
            assert len(json.dumps(large_response, separators=(',', ':')).encode()) == 1048576
            send(plugin, large_response)
            # receive() bounds the actual Hub line, including after external ID restoration.
            response = receive(hub_stream)
            assert response['type'] == 'response' and response['id'] == long_id and response['route'] == rule['route'], response
            assert response['ok'] is False and response['error']['code'] == 'response_too_large', response
            send(hub_client, dict(type='request', id='after-large-response', route=rule['route'], method='echo'))
            continued = expected(plugin_stream, 'request')
            send(plugin, dict(type='response', id=continued['id'], route=rule['route'], ok=True, result='continued'))
            assert receive(hub_stream) == dict(type='response', id='after-large-response', route=rule['route'],
                                               ok=True, result='continued')
            status = control('status')
            assert status['pending_jobs'] == 0 and status['plugin_count'] == 1 and status['hub_connected'], status
            print('PASS: wire ID echo, timeout ID reuse rejects stale response, invalid method preserves active work, '
                  'and 1 MiB plugin response with 128-character Hub ID returns bounded response_too_large and continues')
            control('route_delete', route=rule['route'])
            status = control('status')
            route = next(row for row in status['routes'] if row['route'] == rule['route'])
            assert route['online'] and not route['configured']
            control('reconnect')
            new_hub, _ = hub.accept()
            sockets.append(new_hub)
            new_hub.settimeout(5)
            new_stream = new_hub.makefile('rb')
            expected(new_stream, 'hello')
            assert expected(new_stream, 'route')['route'] == rule['route']
            if args.instrumentation:
                # Root provisions a temporary private endpoint fixture. This tests the
                # production client as the real app UID/domain without changing su grants.
                app_data = '/data/user/0/' + args.manager_package
                uid = int(root('stat -c %u ' + shlex.quote(app_data)))
                fixture = app_data + '/files/.router-smoke-' + suffix + '.json'
                root('mkdir -p ' + shlex.quote(app_data + '/files') + '; '
                     + 'chown ' + str(uid) + ':' + str(uid) + ' ' + shlex.quote(app_data + '/files') + '; '
                     + 'chmod 700 ' + shlex.quote(app_data + '/files') + '; '
                     + 'cp ' + shlex.quote(device_dir + '/control.json') + ' ' + shlex.quote(fixture) + '; '
                     + 'chown ' + str(uid) + ':' + str(uid) + ' ' + shlex.quote(fixture) + '; '
                     + 'chmod 600 ' + shlex.quote(fixture) + '; '
                     + 'restorecon -F ' + shlex.quote(app_data + '/files') + ' ' + shlex.quote(fixture))
                instrument = run('shell', 'am instrument -w -r -e endpoint_fixture '
                                 + shlex.quote(fixture) + ' ' + shlex.quote(args.instrumentation))
                assert 'router_status=passed' in instrument and 'INSTRUMENTATION_CODE: -1' in instrument, instrument
                print('PASS: production RouterClient Unix authentication and metadata caching under real Manager app UID/SELinux domain (private fixture discovery)')
            # Control-client closure does not own daemon lifetime.
            time.sleep(.3)
            assert control('status')['pid'] == pid
            print('PASS: Android daemon, private metadata, named Unix sockets, authenticated status, route CRUD, forwarding, deadline, reconnect and independent lifetime')
    finally:
        for sock in sockets:
            sock.close()
        if fixture is not None:
            root('rm -f ' + shlex.quote(fixture))
        if pid is not None:
            root(f'if [ "$(readlink /proc/{pid}/exe)" = {shlex.quote(device_bin)} ]; then kill {pid}; fi')
        root('rm -rf ' + shlex.quote(device_dir))
        for local in forwards:
            run('forward', '--remove', f'tcp:{local}')
        if reverse_port is not None:
            run('reverse', '--remove', f'tcp:{reverse_port}')


if __name__ == '__main__':
    main()
