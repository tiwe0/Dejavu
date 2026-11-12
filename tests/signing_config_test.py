#!/usr/bin/env python3
"""CI signing gate regressions using synthetic bytes, never actual private keys."""
import base64
import contextlib
import importlib.util
import io
from pathlib import Path
import stat
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('dejavu_ci_signing', ROOT / 'app/scripts/prepare-signing.py')
signing = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(signing)


class SigningConfigTest(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory(prefix='dejavu-signing-test-')
        self.path = Path(self.work.name)
        self.environment = {'RUNNER_TEMP': str(self.path)}

    def tearDown(self):
        self.work.cleanup()

    def complete(self):
        self.environment.update(dict(zip(signing.NAMES, (
            base64.b64encode(b'synthetic keystore, not a private key').decode(),
            'synthetic-store-password', 'synthetic-alias', 'synthetic-key-password'))))
        return self.environment

    def test_ordinary_ci_can_build_an_explicit_debug_artifact(self):
        self.assertEqual(signing.prepare(self.environment), {'variant': 'Debug', 'signed': 'false', 'keystore': None})
        self.assertEqual(list(self.path.iterdir()), [])

    def test_publication_without_a_stable_key_is_rejected(self):
        self.environment['REQUIRE_RELEASE_SIGNING'] = 'true'
        with self.assertRaisesRegex(ValueError, 'publication requires'):
            signing.prepare(self.environment)
        self.assertEqual(list(self.path.iterdir()), [])

    def test_partial_secrets_cannot_fall_back_to_debug(self):
        for missing in signing.NAMES:
            environment = self.complete().copy()
            del environment[missing]
            with self.subTest(missing=missing), self.assertRaisesRegex(ValueError, 'Incomplete release signing'):
                signing.prepare(environment)
        self.assertEqual(list(self.path.iterdir()), [])

    def test_invalid_or_empty_key_bytes_leave_no_file(self):
        for encoded in ('not-base64!', '', base64.b64encode(b'').decode()):
            environment = self.complete().copy()
            environment[signing.NAMES[0]] = encoded
            with self.subTest(encoded=encoded), self.assertRaises(ValueError):
                signing.prepare(environment)
        self.assertEqual(list(self.path.iterdir()), [])

    def test_complete_signing_creates_private_unique_key_and_cleanup(self):
        captured = io.StringIO()
        with contextlib.redirect_stdout(captured):
            selected = signing.prepare(self.complete())
        self.assertEqual(selected['variant'], 'Release')
        self.assertEqual(selected['signed'], 'true')
        key = Path(selected['keystore'])
        self.assertEqual(key.parent, self.path.resolve())
        self.assertEqual(stat.S_IMODE(key.stat().st_mode), 0o600)
        self.assertEqual(key.read_bytes(), b'synthetic keystore, not a private key')
        self.assertEqual(captured.getvalue(), '')
        signing.cleanup({**self.environment, 'DEJAVU_SIGNING_KEYSTORE_FILE': str(key)})
        self.assertFalse(key.exists())

    def test_cleanup_cannot_remove_a_user_supplied_keystore(self):
        key = self.path / 'unrelated.jks'
        key.write_bytes(b'unrelated')
        with self.assertRaisesRegex(ValueError, 'Refusing to remove'):
            signing.cleanup({**self.environment, 'DEJAVU_SIGNING_KEYSTORE_FILE': str(key)})
        self.assertTrue(key.exists())

    def test_cleanup_rejects_a_matching_name_outside_job_scope(self):
        with tempfile.TemporaryDirectory(prefix='dejavu-signing-other-') as other:
            key = Path(other) / 'dejavu-signing-test.jks'
            key.write_bytes(b'unrelated')
            with self.assertRaisesRegex(ValueError, 'outside this job'):
                signing.cleanup({**self.environment, 'DEJAVU_SIGNING_KEYSTORE_FILE': str(key)})
            self.assertTrue(key.exists())

    def test_cli_exports_only_key_path_and_non_secret_variant_outputs(self):
        environment = self.complete().copy()
        environment['GITHUB_ENV'] = str(self.path / 'github-env')
        environment['GITHUB_OUTPUT'] = str(self.path / 'github-output')
        captured = io.StringIO()
        with mock.patch.dict(signing.os.environ, environment, clear=True), \
                mock.patch.object(sys, 'argv', ['prepare-signing.py']), contextlib.redirect_stdout(captured):
            signing.main()
        exported = Path(environment['GITHUB_ENV']).read_text()
        outputs = Path(environment['GITHUB_OUTPUT']).read_text()
        self.assertTrue(exported.startswith('DEJAVU_SIGNING_KEYSTORE_FILE='))
        self.assertEqual(len(exported.splitlines()), 1)
        self.assertEqual(outputs, 'variant=Release\nsigned=true\n')
        for name in signing.NAMES:
            self.assertNotIn(environment[name], exported + outputs + captured.getvalue())
        key = exported.strip().split('=', 1)[1]
        signing.cleanup({**environment, 'DEJAVU_SIGNING_KEYSTORE_FILE': key})


if __name__ == '__main__':
    unittest.main()
