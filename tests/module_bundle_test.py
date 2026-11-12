#!/usr/bin/env python3
"""JDK-only regressions for the module payload embedded in the Dejavu app."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import warnings
import zipfile

ROOT = Path(__file__).resolve().parents[1]
REQUIRED = (
    'customize.sh', 'service.sh', 'uninstall.sh', 'root-watchdog.sh',
    'router-control.sh', 'router-install.sh', 'router-watchdog.sh', 'router-config.example.json',
    'zygisk/arm64-v8a.so', 'lib/arm64-v8a/libdejavu_agent.so', 'lib/arm64-v8a/libdejavu.so',
    'lib/arm64-v8a/dejavu-root-helper', 'bin/dejavuerouter', 'config/init.lua', 'config/targets.txt',
)
PROP = b'id=dejavu_zygisk\nversion=v0.9.1\nversionCode=12\n'


class ModuleBundleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='dejavu-module-tool-')
        cls.classes = Path(cls.build.name)
        java_home = os.environ.get('JAVA_HOME')
        cls.java = str(Path(java_home) / 'bin/java') if java_home else shutil.which('java')
        javac = str(Path(java_home) / 'bin/javac') if java_home else shutil.which('javac')
        if not cls.java or not javac:
            raise RuntimeError('JDK 17+ is required; set JAVA_HOME')
        subprocess.run([javac, '--release', '17', '-d', str(cls.classes),
                        str(ROOT / 'app/scripts/ModuleBundleTool.java')], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.work = tempfile.TemporaryDirectory(prefix='dejavu-module-bundle-')
        self.path = Path(self.work.name)
        self.props = self.path / 'module.prop'
        self.props.write_bytes(PROP)
        self.archive = self.path / 'module.zip'
        self.output = self.path / 'assets'

    def tearDown(self):
        self.work.cleanup()

    def make_zip(self, prop=PROP, extras=(), omitted=()):
        with warnings.catch_warnings():
            warnings.simplefilter('ignore', UserWarning)
            with zipfile.ZipFile(self.archive, 'w', compression=zipfile.ZIP_STORED) as zip:
                zip.writestr('module.prop', prop)
                for name in REQUIRED:
                    if name not in omitted:
                        zip.writestr(name, b'payload')
                for name, content in extras:
                    zip.writestr(name, content)

    def prepare(self):
        return subprocess.run([self.java, '-cp', str(self.classes), 'ModuleBundleTool',
                               str(self.archive), str(self.props), str(self.output)],
                              text=True, capture_output=True, timeout=15)

    def assert_rejected(self, detail=None):
        self.output.mkdir(exist_ok=True)
        sentinel = self.output / 'prior-assets'
        sentinel.write_bytes(b'previous known-good bundle')
        result = self.prepare()
        self.assertNotEqual(result.returncode, 0, result.stdout)
        if detail:
            self.assertIn(detail, result.stderr)
        self.assertEqual(sentinel.read_bytes(), b'previous known-good bundle')
        self.assertEqual(sorted(p.name for p in self.path.iterdir()),
                         sorted(p for p in ('assets', 'module.prop', 'module.zip') if (self.path / p).exists()))

    def test_exact_zip_metadata_and_replacement(self):
        self.make_zip(extras=(('LICENSES/', b''), ('LICENSES/example.txt', b'license')))
        self.output.mkdir()
        (self.output / 'obsolete').write_text('old')
        result = self.prepare()
        self.assertEqual(result.returncode, 0, result.stderr)
        embedded = (self.output / 'dejavu/module.zip').read_bytes()
        self.assertEqual(embedded, self.archive.read_bytes())
        self.assertEqual(json.loads((self.output / 'dejavu/module.json').read_text()), dict(
            id='dejavu_zygisk', version='v0.9.1', versionCode=12,
            sha256=hashlib.sha256(embedded).hexdigest(), size=len(embedded)))
        self.assertFalse((self.output / 'obsolete').exists())

    def test_missing_zip_has_actionable_error_and_preserves_old_output(self):
        self.assert_rejected('Run ./dejavu package first or set -PdejavuModuleZip=')

    def test_invalid_zip_preserves_old_output(self):
        self.archive.write_bytes(b'not a zip')
        self.assert_rejected()

    def test_wrong_identity_or_version_cannot_enter_app(self):
        for prop in (PROP.replace(b'dejavu_zygisk', b'other_module'),
                     PROP.replace(b'v0.9.1', b'v0.9.0'), PROP.replace(b'versionCode=12', b'versionCode=11')):
            with self.subTest(prop=prop):
                self.make_zip(prop=prop)
                self.assert_rejected('does not match')

    def test_unsafe_paths_and_duplicate_entry_are_rejected(self):
        for entry in ('../escape', '/absolute', 'a/../escape', 'a\\b', 'C:/file', 'a//b',
                      'a/./b', 'a\nb', 'module.prop'):
            with self.subTest(entry=entry):
                self.make_zip(extras=((entry, b'invalid'),))
                self.assert_rejected()

    def test_file_directory_collision_is_rejected(self):
        self.make_zip(extras=(('bin', b'file, not directory'),))
        self.assert_rejected('file/directory collision')

    def test_incomplete_module_is_rejected(self):
        self.make_zip(omitted=('lib/arm64-v8a/libdejavu_agent.so',))
        self.assert_rejected('Missing module payload')

    def test_duplicate_property_keys_are_rejected(self):
        self.make_zip(prop=PROP + b'id=dejavu_zygisk\n')
        self.assert_rejected('Duplicate module.prop key')

    def test_corrupt_payload_crc_is_rejected(self):
        self.make_zip()
        with zipfile.ZipFile(self.archive) as zip:
            entry = zip.getinfo('bin/dejavuerouter')
        content = bytearray(self.archive.read_bytes())
        offset = entry.header_offset + 30 + len(entry.filename.encode())
        content[offset] ^= 0x1
        self.archive.write_bytes(content)
        self.assert_rejected('CRC')

    def test_declared_expansion_limit_is_checked_before_decompression(self):
        self.make_zip()
        content = bytearray(self.archive.read_bytes())
        offset = content.index(b'PK\x01\x02')
        struct.pack_into('<I', content, offset + 24, 128 * 1024 * 1024 + 1)
        self.archive.write_bytes(content)
        self.assert_rejected('limit exceeded')


if __name__ == '__main__':
    unittest.main()
