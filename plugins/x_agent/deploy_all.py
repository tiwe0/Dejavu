#!/usr/bin/env python3
import argparse, hashlib, json, os, shlex, socket, struct, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PLUGIN = ROOT / 'out/plugins/x_agent/libdejavu_x_agent.so'
SERIAL = os.environ.get('ADB_SERIAL', 'c44d68aa')
ADB_ENV = dict(os.environ, ADB_SERVER_SOCKET='tcp:127.0.0.1:5037')
MAX_PAYLOAD = 1024 * 1024
RPC_HEADER = struct.Struct('<IIIiQI')
FRAME_HEADER = struct.Struct('<I')
MAGIC = 0x444A5250
VERSION = 1
EXEC_REQUEST = 1
EXEC_RESPONSE = 2
RPC_OK = 0

def adb(*args, check=False):
    return subprocess.run(['adb', '-s', SERIAL, *args], env=ADB_ENV,
                          capture_output=True, text=True, check=check, timeout=60)

def root(command):
    return adb('shell', 'su', '-c', command)

def remote_read(path):
    result = root('cat ' + shlex.quote(path))
    if result.returncode != 0:
        raise RuntimeError(f'root cat failed: {path}: {result.stderr.strip()}')
    return result.stdout

def push(local, remote):
    result = adb('push', str(local), remote)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip())

def run_remote_script(text, name):
    path = Path('/tmp') / name
    path.write_text(text)
    push(path, '/data/local/tmp/' + name)
    result = root('sh /data/local/tmp/' + name)
    if result.returncode != 0:
        print('remote script failed:', result.stdout[-2000:], result.stderr[-2000:])
        raise RuntimeError('remote script failed')
    return result.stdout

def read_exact(conn, size):
    out = b''
    while len(out) < size:
        part = conn.recv(size - len(out))
        if not part:
            raise ConnectionError('control connection closed')
        out += part
    return out

def read_frame(conn, maximum):
    (size,) = FRAME_HEADER.unpack(read_exact(conn, FRAME_HEADER.size))
    if size > maximum:
        raise RuntimeError('RPC frame too large')
    return read_exact(conn, size)

def write_frame(conn, payload):
    conn.sendall(FRAME_HEADER.pack(len(payload)) + payload)

def control_execute(pid, script):
    meta_path = f'/data/adb/modules/dejavu_zygisk/run/com.twitter.android.{pid}.rpc'
    meta = json.loads(remote_read(meta_path))
    sock_name = meta['socket']
    token = bytes.fromhex(meta['token'])
    fwd = adb('forward', 'tcp:0', 'localabstract:' + sock_name)
    if fwd.returncode != 0:
        raise RuntimeError('adb forward failed: ' + fwd.stderr.strip())
    port = int(fwd.stdout.strip())
    try:
        conn = socket.create_connection(('127.0.0.1', port), timeout=20)
        try:
            conn.settimeout(20)
            write_frame(conn, token)
            if read_frame(conn, 0):
                raise RuntimeError('invalid auth ack')
            request_id = int(time.time() * 1000) & 0x7fffffff
            request = RPC_HEADER.pack(MAGIC, VERSION, EXEC_REQUEST, RPC_OK,
                                      request_id, len(script)) + script.encode()
            write_frame(conn, request)
            response = read_frame(conn, RPC_HEADER.size + MAX_PAYLOAD)
            if len(response) < RPC_HEADER.size:
                raise RuntimeError('truncated RPC response')
            magic, ver, typ, status, rid, size = RPC_HEADER.unpack(response[:RPC_HEADER.size])
            payload = response[RPC_HEADER.size:]
            if magic != MAGIC or ver != VERSION or typ != EXEC_RESPONSE or rid != request_id or size != len(payload):
                raise RuntimeError('invalid RPC response')
            return status, payload.decode('utf-8', 'replace')
        finally:
            conn.close()
    finally:
        adb('forward', '--remove', f'tcp:{port}')

def lua_quote(value):
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'

def discover():
    result = adb('shell', 'ps', '-A', '-o', 'PID,UID,NAME')
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip())
    found = {}
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) < 3 or parts[2] != 'com.twitter.android':
            continue
        pid, uid = int(parts[0]), int(parts[1])
        user = uid // 100000
        if user not in found:
            found[user] = (pid, uid)
    return found

def main():
    if not PLUGIN.is_file():
        raise SystemExit('build plugin first')
    plugin_hash = hashlib.sha256(PLUGIN.read_bytes()).hexdigest()
    print('plugin_hash', plugin_hash)
    processes = discover()
    print('processes', processes)
    if len(processes) != 8:
        print('warning: expected 8 X users, got', len(processes))
    push(PLUGIN, '/data/local/tmp/libdejavu_x_agent.so')

    copy_lines = ['set -e']
    plugin_paths = {}
    for user, (pid, uid) in sorted(processes.items()):
        base = f'/data/user/{user}/com.twitter.android'
        path = base + '/files/libdejavu_x_agent.so'
        plugin_paths[user] = path
        copy_lines.append(f'mkdir -p {base}/files')
        copy_lines.append(f'cp /data/local/tmp/libdejavu_x_agent.so {path}')
        copy_lines.append(f'chown {uid}:{uid} {path}')
        copy_lines.append(f'chmod 755 {path}')
        copy_lines.append(f'chcon u:object_r:apk_data_file:s0 {path} 2>/dev/null || true')
    run_remote_script('\n'.join(copy_lines) + '\n', 'x_agent_copy.sh')
    print('copied plugin to', len(plugin_paths), 'users')

    xsvc_path = '/data/user_de/0/com.twitter.android/files/libdejavu_xsvc.so'
    xsvc_hash = root('getprop debug.dejavu.plugins.sha256.0').stdout.strip()
    paths = [xsvc_path] + [plugin_paths[u] for u in sorted(plugin_paths)]
    hashes = [xsvc_hash] + [plugin_hash] * len(plugin_paths)
    lines = ['set -e', "setprop debug.dejavu.plugins.allowed ''"]
    for index in range(16):
        path = paths[index] if index < len(paths) else ''
        digest = hashes[index] if index < len(hashes) else ''
        lines.append(f"setprop debug.dejavu.plugins.allowed.{index} {shlex.quote(path)}")
        lines.append(f"setprop debug.dejavu.plugins.sha256.{index} {shlex.quote(digest)}")
        lines.append(f"setprop debug.dejavu.plugins.skill.sha256.{index} ''")
    run_remote_script('\n'.join(lines) + '\n', 'x_agent_allow.sh')
    print('allowlist updated', len(paths), 'paths')

    for user, (pid, uid) in sorted(processes.items()):
        path = plugin_paths[user]
        script = ('local ok, err = pcall(function() plugin.unload("x_agent") end)\n'
                  f'return plugin.load({lua_quote(path)})\n')
        try:
            status, payload = control_execute(pid, script)
            print('load', user, 'pid', pid, 'status', status, 'payload', payload[:120])
        except Exception as error:
            print('load FAILED', user, 'pid', pid, type(error).__name__, str(error)[:240])

    # Observe routes and call token through the ordinary server facade.
    sys.path.insert(0, str(ROOT / 'dejavurouter/tools'))
    from hub_poc import RouterHub
    hub = RouterHub(port=39010).start()
    try:
        if not hub.wait_connected(10):
            print('hub not connected')
            return 1
        targets = json.loads((ROOT / 'dejavurouter/android/module/wake_targets.json').read_text())
        for account in targets:
            hub.bind_account(account, 'c44d68aa', account)
        deadline = time.time() + 20
        while time.time() < deadline and len(hub.routes) < 8:
            time.sleep(0.1)
        print('routes', json.dumps(hub.routes, ensure_ascii=False))
        ok = 0
        for account in targets:
            try:
                token = hub.call_account(account, 'token', timeout=10)
                print('token', account, 'len', len(token.get('token','')))
                ok += 1
            except Exception as error:
                print('token FAILED', account, type(error).__name__, str(error)[:160])
        print('token_ok', ok, 'route_count', len(hub.routes))
        return 0 if ok == len(targets) and len(hub.routes) >= 8 else 1
    finally:
        hub.stop()

if __name__ == '__main__':
    raise SystemExit(main())