#!/usr/bin/env python3
"""Verify internal deployment and lifecycle in an isolated Android directory."""
import argparse
import json
from pathlib import Path
import secrets
import shlex
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', default='adb')
    parser.add_argument('--serial', required=True)
    parser.add_argument('--binary', type=Path, required=True)
    args = parser.parse_args()
    adb = [args.adb, '-s', args.serial]
    suffix = secrets.token_hex(6)
    directory = '/data/local/tmp/dejavu-router-layout-' + suffix
    module = directory + '/module'
    shared = directory + '/dejavu'
    binary = shared + '/bin/dejavuerouter'
    payload = module + '/bin/dejavuerouter'
    data = shared + '/router'
    configured = False

    def run(*arguments):
        return subprocess.check_output(adb + list(arguments), text=True, timeout=30).strip()

    def root(command):
        return run('shell', 'su -M -c ' + shlex.quote(command))

    def control(action):
        command = 'DEJAVU_DIR=' + shlex.quote(shared) + ' sh ' + shlex.quote(module + '/router-control.sh')
        return json.loads(root(command + ' ' + action))

    def install(action='install'):
        root('DEJAVU_DIR=' + shlex.quote(shared) + ' sh ' + shlex.quote(module + '/router-install.sh') + ' ' + action)

    try:
        assert root('id -u') == '0', 'Root access required'
        run('shell', 'mkdir -p ' + shlex.quote(module + '/bin'))
        run('push', str(args.binary.resolve()), payload)
        for name in ('router-install.sh', 'router-control.sh', 'router-watchdog.sh', 'router-config.example.json'):
            run('push', str(ROOT / 'zygisk/module' / name), module + '/' + name)
        with tempfile.TemporaryDirectory(prefix='dejavu-router-layout-') as temporary:
            config = Path(temporary) / 'config.json'
            config.write_text(json.dumps(dict(hub_host='', hub_port=39010, device_id='layout-test', listen_tcp=False,
                                             control_socket='dejavu.router.layout.control.' + suffix,
                                             plugin_socket='dejavu.router.layout.plugins.' + suffix)))
            run('push', str(config), module + '/router-config.example.json')
        root('chown -R 0:0 ' + shlex.quote(directory) + '; chmod 700 ' + shlex.quote(directory)
             + '; chmod 755 ' + shlex.quote(payload))
        configured = True
        assert not control('status')['installed']
        install()
        assert root('stat -c %a ' + shlex.quote(binary)) == '700'
        assert root('stat -c %a ' + shlex.quote(shared + '/bin')) == '700'
        assert control('boot') == dict(installed=True, running=False, enabled=False, pid=0)
        first = control('start')
        assert first['running'] and first['enabled']
        pid = first['pid']
        assert root(f'readlink /proc/{pid}/exe') == binary
        assert control('start')['pid'] == pid
        root("printf '\\n' >> " + shlex.quote(payload))
        install()
        assert root(f'readlink /proc/{pid}/exe') == binary + ' (deleted)'
        assert control('status')['pid'] == pid
        assert control('start')['pid'] == pid
        assert control('stop') == dict(installed=True, running=False, enabled=False, pid=0)
        assert not control('boot')['running']
        restarted = control('start')
        pid = restarted['pid']
        assert root(f'readlink /proc/{pid}/exe') == binary
        root(f'if [ "$(readlink /proc/{pid}/exe)" = {shlex.quote(binary)} ]; then kill -KILL {pid}; fi')
        for _ in range(40):
            status = control('status')
            if status['running'] and status['pid'] != pid:
                break
            time.sleep(.1)
        else:
            raise AssertionError('Watchdog did not recover the internal executable')
        assert root(f"readlink /proc/{status['pid']}/exe") == binary
        assert not control('stop')['enabled']
        time.sleep(2.2)
        assert not control('status')['running']
        install('remove')
        assert not control('status')['installed']
        assert root('test -f ' + shlex.quote(data + '/config.json') + ' && echo retained') == 'retained'
        print('PASS: internal bin layout, private permissions, boot opt-in, idempotent start, atomic upgrade, stop of replaced executable, crash recovery, durable stop, uninstall preserves config')
    finally:
        if configured:
            control('stop')
        root('rm -rf ' + shlex.quote(directory))


if __name__ == '__main__':
    main()
