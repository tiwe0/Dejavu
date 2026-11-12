#!/usr/bin/env python3
"""Provision a CI release key from environment; never print signing secrets."""
import argparse
import base64
import binascii
import os
from pathlib import Path
import tempfile

NAMES = (
    'DEJAVU_SIGNING_KEYSTORE_BASE64', 'DEJAVU_SIGNING_STORE_PASSWORD',
    'DEJAVU_SIGNING_KEY_ALIAS', 'DEJAVU_SIGNING_KEY_PASSWORD',
)


def prepare(environment):
    values = [environment.get(name, '') for name in NAMES]
    if any(values) and not all(values):
        raise ValueError('Incomplete release signing secrets; configure all four DEJAVU_SIGNING_* repository secrets')
    if not all(values):
        if environment.get('REQUIRE_RELEASE_SIGNING') == 'true':
            raise ValueError('Release publication requires all four DEJAVU_SIGNING_* repository secrets')
        return {'variant': 'Debug', 'signed': 'false', 'keystore': None}
    try:
        decoded = base64.b64decode(''.join(values[0].split()), validate=True)
    except (ValueError, binascii.Error):
        raise ValueError('Release signing keystore is not valid base64') from None
    if not decoded or len(decoded) > 1024 * 1024:
        raise ValueError('Release signing keystore size is invalid')
    directory = Path(environment['RUNNER_TEMP']).resolve()
    if any(c in str(directory) for c in '\r\n'):
        raise ValueError('Invalid runner temporary directory')
    directory.mkdir(parents=True, exist_ok=True)
    descriptor, path = tempfile.mkstemp(prefix='dejavu-signing-', suffix='.jks', dir=directory)
    try:
        with os.fdopen(descriptor, 'wb') as stream:
            os.fchmod(stream.fileno(), 0o600)
            stream.write(decoded)
    except BaseException:
        Path(path).unlink(missing_ok=True)
        raise
    return {'variant': 'Release', 'signed': 'true', 'keystore': path}


def cleanup(environment):
    value = environment.get('DEJAVU_SIGNING_KEYSTORE_FILE')
    if not value:
        return
    path = Path(value)
    directory = Path(environment['RUNNER_TEMP']).resolve()
    if path.parent.resolve() != directory or not path.name.startswith('dejavu-signing-') or path.suffix != '.jks':
        raise ValueError('Refusing to remove a key outside this job temporary signing scope')
    path.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cleanup', action='store_true')
    args = parser.parse_args()
    if args.cleanup:
        cleanup(os.environ)
        return
    selected = prepare(os.environ)
    try:
        with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as output:
            output.write('variant=' + selected['variant'] + '\nsigned=' + selected['signed'] + '\n')
        if selected['keystore']:
            with open(os.environ['GITHUB_ENV'], 'a', encoding='utf-8') as output:
                output.write('DEJAVU_SIGNING_KEYSTORE_FILE=' + selected['keystore'] + '\n')
    except BaseException:
        if selected['keystore']:
            Path(selected['keystore']).unlink(missing_ok=True)
        raise
    print('Selected ' + selected['variant'] + ' app build; signing credentials are not written to properties')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, KeyError) as error:
        raise SystemExit(str(error)) from None
