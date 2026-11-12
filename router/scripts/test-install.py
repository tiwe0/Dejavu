#!/usr/bin/env python3
"""Portable installer checks without touching the host's /data or Android device."""
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='dejavu-router-install-') as temporary:
    test = Path(temporary)
    module = test / 'module'
    shared = test / 'dejavu'
    (module / 'bin').mkdir(parents=True)
    shutil.copyfile(ROOT / 'zygisk/module/router-install.sh', module / 'router-install.sh')
    payload = module / 'bin/dejavuerouter'
    payload.write_bytes(b'#!/bin/sh\nexit 0\n')
    payload.chmod(0o755)
    binary = shared / 'bin/dejavuerouter'
    env = dict(os.environ, DEJAVU_DIR=str(shared))
    command = ['sh', str(module / 'router-install.sh')]
    def install(ok=True, action='install'):
        result = subprocess.run(command + [action], env=env, capture_output=True, text=True, timeout=5)
        assert (result.returncode == 0) == ok, result.stderr
    install()
    assert binary.read_bytes() == payload.read_bytes()
    assert stat.S_IMODE(binary.stat().st_mode) == 0o700
    assert stat.S_IMODE(binary.parent.stat().st_mode) == 0o700
    inode = binary.stat().st_ino
    install()
    assert binary.stat().st_ino == inode, 'unchanged binary must not be replaced'
    with binary.open('rb') as running:
        payload.write_bytes(b'#!/bin/sh\nexit 1\n')
        install()
        assert binary.stat().st_ino != inode
        assert running.read() == b'#!/bin/sh\nexit 0\n', 'upgrade must preserve existing mappings'
    assert binary.read_bytes() == payload.read_bytes()
    assert not list(binary.parent.glob('.dejavuerouter.*'))
    (shared / 'router').mkdir()
    config = shared / 'router/config.json'
    config.write_text('{"device_id":"test"}')
    payload.unlink()
    before = binary.read_bytes()
    install(ok=False)
    assert binary.read_bytes() == before, 'invalid payload must not destroy installed binary'
    install(action='remove')
    assert not binary.exists() and config.exists()
    # Refuse aliased shared directories rather than deploying to a different location.
    alias = test / 'alias'
    alias.symlink_to(shared, target_is_directory=True)
    env['DEJAVU_DIR'] = str(alias)
    install(ok=False)
print('PASS: internal bin path, permissions, idempotence, atomic upgrade, failed install, removal preserves config, symlink rejection')
