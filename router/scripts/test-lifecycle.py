#!/usr/bin/env python3
"""Rootless lifecycle regression tests on Linux, using an isolated fake module."""
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time

if not Path('/proc/self/stat').exists():
    print('SKIP: lifecycle process-identity tests require Linux /proc')
    sys.exit(0)

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='dejavu-router-lifecycle-') as temporary:
    base = Path(temporary)
    module = base / 'module'
    data = base / 'data'
    (module / 'bin').mkdir(parents=True)
    shared = base / 'dejavu'
    for name in ('router-install.sh', 'router-control.sh', 'router-watchdog.sh', 'router-config.example.json'):
        shutil.copyfile(ROOT / 'zygisk/module' / name, module / name)
    source = base / 'stub.c'
    source.write_text(r'''
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static volatile sig_atomic_t live = 1;
static void stop(int sig) { (void)sig; live = 0; }
int main(int argc, char **argv) {
    if (argc != 2 || getenv("DEJAVU_TEST_FAIL")) return 2;
    char path[4096]; snprintf(path, sizeof(path), "%s", argv[1]);
    char *slash = strrchr(path, '/'); if (!slash) return 2;
    strcpy(slash + 1, "control.json");
    signal(SIGTERM, stop); signal(SIGINT, stop);
    FILE *f = fopen(path, "w"); if (!f) return 3;
    fputs("{\"host\":\"127.0.0.1\",\"port\":39112,\"token\":\"test\"}\n", f);
    fclose(f); while (live) pause(); return 0;
}
''')
    payload = module / 'bin/dejavuerouter'
    binary = shared / 'bin/dejavuerouter'
    subprocess.run([os.environ.get('CC', 'cc'), str(source), '-o', str(payload)], check=True)
    env = dict(os.environ, DEJAVU_DIR=str(shared), DEJAVU_ROUTER_DATA_DIR=str(data), DEJAVU_ROUTER_SHELL='/bin/sh')
    install = ['/bin/sh', str(module / 'router-install.sh')]
    subprocess.run(install, env=env, check=True)
    command = ['/bin/sh', str(module / 'router-control.sh')]
    def call(action, check=True, extra=None):
        result = subprocess.run(command + [action], env=dict(env, **(extra or {})),
                                text=True, capture_output=True, timeout=15)
        if check:
            assert result.returncode == 0, (action, result.stdout, result.stderr)
            return json.loads(result.stdout)
        return result
    def pid_start(pid):
        return Path(f'/proc/{pid}/stat').read_text().split()[21]
    try:
        assert call('boot') == dict(installed=True, running=False, enabled=False, pid=0)
        first = call('start'); assert first['running'] and first['enabled']
        assert call('start')['pid'] == first['pid'], 'idempotent start launched another daemon'
        processes = [subprocess.Popen(command + ['start'], env=env, text=True,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE) for _ in range(6)]
        for process in processes:
            out, err = process.communicate(timeout=15)
            assert process.returncode == 0, err
            assert json.loads(out)['pid'] == first['pid']
        assert os.readlink(f"/proc/{first['pid']}/exe") == str(binary)
        # Atomic upgrades retain control of a live instance whose old inode was replaced.
        with payload.open('ab') as output:
            output.write(b'\n')
        subprocess.run(install, env=env, check=True)
        assert os.readlink(f"/proc/{first['pid']}/exe") == str(binary) + ' (deleted)'
        assert call('status')['pid'] == first['pid']
        assert call('start')['pid'] == first['pid']
        os.kill(first['pid'], signal.SIGKILL)
        until = time.monotonic() + 6
        while time.monotonic() < until:
            state = call('status')
            if state['running'] and state['pid'] != first['pid']:
                break
            time.sleep(.1)
        else:
            raise AssertionError('watchdog did not restart crashed daemon')
        assert call('stop') == dict(installed=True, running=False, enabled=False, pid=0)
        time.sleep(2.2)
        assert not call('status')['running'], 'explicit stop restarted daemon'
        assert not call('boot')['running'], 'disabled router resumed at boot'
        # Even a live unrelated PID with a correct start-time record is never signalled.
        sentinel = subprocess.Popen(['/bin/sleep', '30'])
        try:
            (data / 'daemon.pid').write_text(f'{sentinel.pid} {pid_start(sentinel.pid)}\n')
            (data / 'watchdog.pid').write_text(f'{sentinel.pid} {pid_start(sentinel.pid)}\n')
            assert not call('status')['running']
            call('stop')
            assert sentinel.poll() is None, 'stop signalled an unrelated process'
        finally:
            sentinel.terminate(); sentinel.wait(timeout=3)
        # A crashed controller leaves a reclaimable lock without affecting other processes.
        (data / 'lifecycle.lock').touch()
        assert call('start')['running']
        assert call('stop')['enabled'] is False
        failed = call('start', check=False, extra={'DEJAVU_TEST_FAIL': '1'})
        assert failed.returncode != 0
        assert not call('status')['enabled'], 'failed startup remained enabled'
        assert not call('status')['running']
        (module / 'disable').touch()
        assert call('start', check=False).returncode != 0
        print('PASS: boot opt-in, idempotence, concurrent start, crash recovery, durable stop, central binary path, atomic upgrade, PID identity, stale lock, startup failure, module disable')
    finally:
        call('stop', check=False)
        subprocess.run(install + ['remove'], env=env, check=True)
        assert not binary.exists()
        assert (data / 'config.json').exists()
