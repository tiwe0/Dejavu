#!/usr/bin/env python3
"""Host regressions for CLI manual ownership and atomic plugin scope updates."""
import argparse
import base64
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader('scope_cli', str(ROOT / 'dejavu'))
spec = importlib.util.spec_from_loader(loader.name, loader)
cli = importlib.util.module_from_spec(spec)
loader.exec_module(cli)


class TargetScopesTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='dejavu-scopes.')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / '.host-only').write_text('scopes-host-only\n')
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        for tool in ('flock', 'sha256sum', 'chown', 'mv'):
            path = self.bin / tool
            shutil.copy(ROOT / 'app/tests/scopes/fake-tool.py', path)
            path.chmod(0o700)
        self.path = self.root / 'data/adb/modules/dejavu_zygisk/config/targets.txt'
        self.path.parent.mkdir(parents=True)
        self.env = dict(os.environ, DEJAVU_SCOPE_SANDBOX=str(self.root), PATH=f'{self.bin}:{os.environ["PATH"]}')
        self.args = argparse.Namespace()
        self.before_write = None
        self.write_count = 0
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(cli, 'root_shell', side_effect=lambda args, command, **kw: self.shell(command)).start()
        mock.patch.object(cli, 'adb', side_effect=self.adb).start()

    def shell(self, command, payload=None):
        command = command.replace('/system/bin/flock', 'flock').replace('/data', str(self.root / 'data'))
        return subprocess.run(['/bin/sh', '-c', command], input=payload, capture_output=True, env=self.env)

    def adb(self, args, *command, **kwargs):
        self.write_count += 1
        if self.before_write:
            callback, self.before_write = self.before_write, None
            callback()
        self.assertEqual(command[0], 'shell')
        parsed = shlex.split(command[1])
        self.assertEqual(parsed[:2], ['su', '-c'])
        return self.shell(parsed[2], kwargs['input_bytes'])

    def seed(self):
        metadata = {'manual': ['manual.app'], 'scopes': {'first': ['shared.app', 'first.app'], 'second': ['shared.app']}}
        self.path.write_text(cli.SCOPE_PREFIX + json.dumps(metadata) + '\nfirst.app\nmanual.app\nshared.app\n')

    def test_manual_clear_preserves_plugin_scopes(self):
        self.seed()
        self.assertEqual(cli.update_manual_targets(self.args, lambda _: []), ['first.app', 'shared.app'])
        _, manual, scopes, marked = cli.read_target_config(self.args)
        self.assertEqual(manual, [])
        self.assertEqual(set(scopes), {'first', 'second'})
        self.assertTrue(marked)
        self.assertEqual(self.path.stat().st_mode & 0o777, 0o600)

    def test_legacy_and_compare_and_swap_retry(self):
        self.path.write_text('legacy.app\n')
        self.before_write = lambda: self.path.write_text('legacy.app\nconcurrent.app\n')
        cli.update_manual_targets(self.args, lambda targets: targets + ['added.app'])
        self.assertEqual(self.write_count, 2)
        self.assertEqual(cli.read_targets(self.args), ['added.app', 'concurrent.app', 'legacy.app'])
        self.assertFalse(cli.read_target_config(self.args)[3])

    def test_failed_rename_preserves_config(self):
        self.seed()
        old = self.path.read_bytes()
        (self.root / 'fail-rename').touch()
        with self.assertRaises(cli.CliError):
            cli.update_manual_targets(self.args, lambda _: [])
        self.assertEqual(self.path.read_bytes(), old)
        self.assertEqual(list(self.path.parent.glob('.targets.*')), [self.path.parent / '.targets.lock'])

    def test_invalid_metadata_size_and_symlink(self):
        for payload in (b'# dejavu-plugin-scopes-v2 {}\n', b'# dejavu-plugin-scopes-v1 {}\n', b'bad target\n', b'#' * 16385, b'\xff'):
            with self.subTest(payload=payload[:40]), self.assertRaises(cli.CliError):
                cli.parse_target_config(payload)
        self.path.symlink_to(self.root / 'victim')
        with self.assertRaises(cli.CliError):
            cli.read_targets(self.args)

    def test_lock_symlink_is_never_opened(self):
        self.seed()
        old = self.path.read_bytes()
        victim = self.root / 'victim'
        victim.write_text('preserve')
        (self.path.parent / '.targets.lock').symlink_to(victim)
        with self.assertRaises(cli.CliError):
            cli.update_manual_targets(self.args, lambda _: [])
        self.assertEqual(victim.read_text(), 'preserve')
        self.assertEqual(self.path.read_bytes(), old)

    def test_metadata_effective_union_must_match(self):
        self.seed()
        self.path.write_text(self.path.read_text() + 'unowned.app\n')
        with self.assertRaises(cli.CliError):
            cli.read_targets(self.args)


if __name__ == '__main__':
    unittest.main()
