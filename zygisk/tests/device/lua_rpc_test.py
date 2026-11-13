#!/usr/bin/env python3
"""Exercise Lua RPC in an already injected Settings process with an isolated Hub.

Deploy matching rebuilt core/Agent first and enable Settings injection. This
script never changes installed router settings or persists request/response bodies.
"""
import argparse
import json
from pathlib import Path
import secrets
import shlex
import socket
import struct
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[3]
MOD = '/data/adb/modules/dejavu_zygisk'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', default='adb')
    parser.add_argument('--serial', required=True)
    parser.add_argument('--router', type=Path, required=True)
    args = parser.parse_args()
    adb = [args.adb, '-s', args.serial]
    suffix = secrets.token_hex(6)
    directory = '/data/local/tmp/dejavu-lua-rpc-' + suffix
    route = 'test.lua.' + suffix
    control_name = 'dejavu.lua.control.' + suffix
    plugin_name = 'dejavu.lua.plugins.' + suffix
    reverse = None
    daemon_identity = None
    daemon_started = False
    executable = directory + '/dejavuerouter'
    package_global = '__dejavu_lua_rpc_device_test_' + suffix
    jni_global = '__dejavu_lua_rpc_jni_test_' + suffix
    target = 'com.android.settings'

    def run(*command):
        return subprocess.check_output(adb + list(command), text=True, timeout=30).strip()

    def root(command):
        return run('shell', 'su -M -c ' + shlex.quote('set -e\n' + command))

    def receive_exact(client, count):
        data = b''
        while len(data) < count:
            chunk = client.recv(count - len(data))
            if not chunk:
                raise AssertionError('Agent disconnected')
            data += chunk
        return data

    def frame(client, data):
        client.sendall(struct.pack('<I', len(data)) + data)

    def read_frame(client):
        size, = struct.unpack('<I', receive_exact(client, 4))
        assert size <= 1048604, 'Agent response exceeds frame limit'
        return receive_exact(client, size)

    pids = []
    for line in root('ps -A -o PID,UID,NAME').splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[2] == target and int(fields[1]) < 100000:
            pids.append(int(fields[0]))
    assert len(pids) == 1, 'Start an injected user-0 Settings process first'
    pid = pids[0]

    def lua(script):
        metadata = json.loads(root('cat ' + MOD + '/run/' + target + '.' + str(pid) + '.rpc'))
        forward = int(run('forward', 'tcp:0', 'localabstract:' + metadata['socket']))
        try:
            with socket.create_connection(('127.0.0.1', forward), timeout=15) as client:
                client.settimeout(15)
                frame(client, bytes.fromhex(metadata['token']))
                assert read_frame(client) == b''
                body = script.encode()
                frame(client, struct.pack('<IIIiQI', 0x444A5250, 1, 1, 0, 1, len(body)) + body)
                reply = read_frame(client)
                magic, version, kind, status, request_id, size = struct.unpack('<IIIiQI', reply[:28])
                assert (magic, version, kind, request_id, size) == (0x444A5250, 1, 2, 1, len(reply) - 28)
                assert status == 0, 'Settings Lua assertion failed'
                return reply[28:].decode()
        finally:
            run('forward', '--remove', 'tcp:' + str(forward))

    def write_config(config):
        script = 'umask 077; cat > ' + shlex.quote(directory + '/config.json')
        subprocess.run(adb + ['shell', '-T', 'su -M -c ' + shlex.quote(script)],
                       input=json.dumps(config).encode(), check=True, capture_output=True, timeout=15)

    def cleanup_lua():
        errors = []
        for key, stop in ((package_global, 'value.stop({})'),
                          (jni_global, 'value:unregister()')):
            try:
                lua('local key=' + json.dumps(key) + '; local value=_G[key]; '
                    'if value then assert(' + stop + '); _G[key]=nil end; return "clean"')
            except Exception as error:
                errors.append(type(error).__name__)
        if errors:
            raise RuntimeError('Lua cleanup failed: ' + ', '.join(errors))

    def process_identity(process_id):
        assert isinstance(process_id, int) and process_id > 1
        try:
            snapshot = root('readlink /proc/' + str(process_id) + '/exe; '
                            'cat /proc/' + str(process_id) + '/stat').splitlines()
        except subprocess.CalledProcessError:
            return None
        if len(snapshot) != 2 or snapshot[0] != executable:
            return None
        stat = snapshot[1]
        assert int(stat.split(' ', 1)[0]) == process_id
        fields = stat.rsplit(')', 1)[1].split()
        assert len(fields) > 19 and fields[19].isdigit()
        return process_id, fields[19]

    def own_processes():
        # A setsid implementation may fork. Locate only this run's exact binary,
        # never processes selected merely by name or a stale launcher PID.
        if not daemon_started:
            return []
        # Filter the task's exact argv[0] before checking /proc identities.
        # Spawning readlink once per device process is prohibitively slow on
        # phones with many app profiles.
        script = ('ps -A -o PID,ARGS | awk -v executable=' + shlex.quote(executable) +
                  ' ' + shlex.quote('$2 == executable {print $1}'))
        identities = []
        for process_id in root(script).splitlines():
            process_id = int(process_id)
            identity = process_identity(process_id)
            if identity is not None:
                identities.append(identity)
        return identities

    def router_identity():
        metadata = json.loads(root('cat ' + shlex.quote(directory + '/control.json')))
        assert metadata['transport'] == 'unix' and metadata['namespace'] == 'abstract'
        assert metadata['name'] == control_name, 'Isolated control endpoint mismatch'
        forward = int(run('forward', 'tcp:0', 'localabstract:' + metadata['name']))
        try:
            with socket.create_connection(('127.0.0.1', forward), timeout=3) as client:
                client.settimeout(3)
                client.sendall(json.dumps({'token': metadata['token'], 'op': 'status'}).encode() + b'\n')
                with client.makefile('rb') as stream:
                    line = stream.readline(1048578)
                assert line.endswith(b'\n') and len(line) <= 1048577
                response = json.loads(line)
                assert response['ok'] is True
                status = response['status']
                assert status['plugin_socket'] == plugin_name, 'Isolated plugin endpoint mismatch'
                identity = process_identity(status['pid'])
                assert identity is not None, 'Router executable identity mismatch'
                return identity
        finally:
            run('forward', '--remove', 'tcp:' + str(forward))

    def stop_router():
        identities = set(own_processes())
        if daemon_identity is not None:
            identities.add(daemon_identity)
        for process_id, start_time in identities:
            # Recheck both executable and start time in the same root command
            # immediately before signalling; a reused PID is never a target.
            proc = '/proc/' + str(process_id)
            start_command = ('sed ' + shlex.quote('s/^.*) //') + ' ' + proc +
                             '/stat 2>/dev/null | awk ' + shlex.quote('{print $20}'))
            root('if [ "$(readlink ' + proc + '/exe 2>/dev/null || true)" = ' +
                 shlex.quote(executable) + ' ] && [ "$(' + start_command + ')" = ' +
                 shlex.quote(start_time) + ' ]; then kill -TERM ' + str(process_id) + '; fi')
        deadline = time.monotonic() + 5
        while own_processes():
            if time.monotonic() >= deadline:
                raise RuntimeError('Isolated router did not exit; retaining its directory')
            time.sleep(.1)

    try:
        assert root('id -u') == '0'
        assert lua("assert(type(rpc.register)=='function' and type(json.encode)=='function'); return 'ready'") == 'ready'
        with socket.socket() as server:
            server.bind(('127.0.0.1', 0))
            server.listen(1)
            server.settimeout(10)
            reverse = run('reverse', 'tcp:0', 'tcp:' + str(server.getsockname()[1]))
            assert reverse.isdigit()
            root('umask 077; mkdir ' + shlex.quote(directory))
            upload = directory + '.upload'
            try:
                run('push', str(args.router.resolve()), upload)
                root('cp ' + shlex.quote(upload) + ' ' + shlex.quote(executable) +
                     '; chown 0:0 ' + shlex.quote(executable) +
                     '; chmod 700 ' + shlex.quote(executable))
            finally:
                run('shell', 'rm -f ' + shlex.quote(upload))
            write_config(dict(hub_host='127.0.0.1', hub_port=int(reverse),
                              device_id='lua-rpc-isolated-test', listen_tcp=False,
                              plugins_dir=directory + '/plugins',
                              plugin_socket=plugin_name, control_socket=control_name))
            daemon_started = True
            root('setsid ' + shlex.quote(executable) + ' ' +
                 shlex.quote(directory + '/config.json') + ' </dev/null >/dev/null 2>&1 &')
            deadline = time.monotonic() + 10
            while daemon_identity is None:
                try:
                    daemon_identity = router_identity()
                except (subprocess.CalledProcessError, OSError):
                    if time.monotonic() >= deadline:
                        raise RuntimeError('Isolated router did not publish control status') from None
                    time.sleep(.1)
            with server.accept()[0] as client:
                client.settimeout(10)
                stream = client.makefile('rb')

                def receive(kind, request_id=None):
                    for _ in range(128):
                        line = stream.readline(1048578)
                        assert line.endswith(b'\n') and len(line) <= 1048577
                        message = json.loads(line)
                        if message.get('type') == kind and (request_id is None or message.get('id') == request_id):
                            return message
                    raise AssertionError('Expected Hub message not received')

                assert receive('hello')['device_id'] == 'lua-rpc-isolated-test'
                source = (ROOT / 'examples/rpc_lua_echo/main.lua').read_text()
                assert source.count('"example.lua.echo"') == 1, 'Example route anchor changed'
                assert 'endpoint' not in source, 'Example endpoint requires an explicit patch'
                assert source.count('name = "rpc_lua_echo"') == 1, 'Example service anchor changed'
                source = source.replace('"example.lua.echo"', json.dumps(route))
                source = source.replace('name = "rpc_lua_echo"',
                                        'name = ' + json.dumps('rpc_lua_echo_' + suffix))
                route_anchor = 'routes = {' + json.dumps(route) + '},'
                assert source.count(route_anchor) == 1, 'Example endpoint anchor changed'
                endpoint = json.dumps('abstract:' + plugin_name)
                source = source.replace(route_anchor, route_anchor + ' endpoint = ' + endpoint + ',')
                assert source.count('endpoint = ' + endpoint) == 1, 'Isolated endpoint missing'
                assert source.count(json.dumps(route)) == 1, 'Isolated route missing'
                lua('local api = assert(load(' + json.dumps(source) +
                    ", '@lua-rpc-device-package', 't', _G))(); "
                    '_G[' + json.dumps(package_global) + '] = api; assert(api.start({})); return \'started\'')
                assert receive('route')['route'] == route

                def request(request_id, method, params):
                    message = dict(type='request', id=request_id, route=route, method=method,
                                   params=params, deadline_ms=3000)
                    client.sendall(json.dumps(message, ensure_ascii=False).encode() + b'\n')
                    return receive('response', request_id)

                params = {'text': 'Lua RPC 中文 😀', 'array': [None, True, 9007199254740991],
                          'negative': -9007199254740991, 'object': {}}
                assert request('echo', 'echo', params)['result'] == params
                assert request('status', 'status', {})['result']['requests'] == 1
                rejected = request('reject', 'fail', {})
                assert rejected['ok'] is False and rejected['error']['code'] == 'example.rejected'
                jni_route = route + '.jni'
                lua('_G[' + json.dumps(jni_global) + '] = rpc.register{name=' +
                    json.dumps('jni_test_' + suffix) + ',app="example.lua_rpc",'
                    'routes={' + json.dumps(jni_route) + '},endpoint=' + json.dumps('abstract:' + plugin_name) + ','
                    'methods={pid=function() return {pid=java.invoke("android.os.Process","myPid","()I")} end}}; '
                    "return 'registered'")
                assert receive('route')['route'] == jni_route
                client.sendall(json.dumps(dict(type='request', id='jni', route=jni_route,
                                                method='pid', params={}, deadline_ms=3000)).encode() + b'\n')
                assert receive('response', 'jni')['result']['pid'] == pid
                cleanup_lua()
    finally:
        original_failure = sys.exc_info()[0] is not None
        errors = []
        router_stopped = False
        for label, cleanup in (('Lua', cleanup_lua), ('router', stop_router)):
            try:
                cleanup()
                if label == 'router':
                    router_stopped = True
            except Exception as error:
                detail = str(error) if isinstance(error, (AssertionError, RuntimeError)) else type(error).__name__
                errors.append(label + ': ' + detail)
        if reverse:
            try:
                run('reverse', '--remove', 'tcp:' + reverse)
            except Exception as error:
                errors.append('reverse: ' + type(error).__name__)
        if router_stopped:
            try:
                root('rm -rf ' + shlex.quote(directory))
            except Exception as error:
                errors.append('directory: ' + type(error).__name__)
        if errors:
            message = 'Cleanup errors: ' + ', '.join(errors)
            if original_failure:
                print(message, file=sys.stderr)
            else:
                raise RuntimeError(message)

    print('PASS: Settings Lua package start/stop, Unix routing, nested JSON, exact safe integers, business errors and JNI callback')


if __name__ == '__main__':
    main()
