#!/usr/bin/env python3
"""Reuse Gradle's Kotlin compiler and prepare the pinned host-test JSON JAR."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import shutil
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
ARTIFACTS = (
    ('json.jar', 'org/json/json/20250517/json-20250517.jar',
     '3ea61b2a06e31edf1c91134fe9106b0ebb16628be169f3db75bc7a2b06b45796'),
)


def checked_artifact(output, relative, repository_path, expected, offline):
    target = output / relative
    if target.is_file():
        if hashlib.sha256(target.read_bytes()).hexdigest() != expected:
            raise ValueError('Host tool checksum mismatch: ' + relative)
        return
    if offline:
        raise ValueError('Missing cached host tool in offline mode: ' + relative)
    target.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix='.download-', dir=target.parent)
    try:
        digest = hashlib.sha256()
        size = 0
        with os.fdopen(descriptor, 'wb') as destination, urllib.request.urlopen(
                'https://repo.maven.apache.org/maven2/' + repository_path, timeout=60) as response:
            while data := response.read(65536):
                size += len(data)
                if size > 128 * 1024 * 1024:
                    raise ValueError('Downloaded host tool exceeds size limit: ' + relative)
                digest.update(data)
                destination.write(data)
        if digest.hexdigest() != expected:
            raise ValueError('Downloaded host tool checksum mismatch: ' + relative)
        Path(temporary).replace(target)
    finally:
        Path(temporary).unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--offline', action='store_true')
    parser.add_argument('--output', type=Path, default=ROOT / 'out/tooling/module-host-ci')
    args = parser.parse_args()
    wrapper = (ROOT / 'gradle/wrapper/gradle-wrapper.properties').read_text()
    match = re.search(r'gradle-([0-9.]+)-bin\.zip', wrapper)
    if not match:
        raise ValueError('Cannot determine Gradle wrapper version for host tests')
    version = match.group(1)
    home = Path(os.environ.get('GRADLE_USER_HOME', Path.home() / '.gradle'))
    candidates = sorted(home.glob(f'wrapper/dists/gradle-{version}-bin/*/gradle-{version}/lib'))
    if not candidates:
        raise ValueError('Run the Gradle app build before preparing the host compiler')
    libraries = candidates[0].resolve()
    output = args.output.resolve()
    (output / 'lib').mkdir(parents=True, exist_ok=True)
    for relative, repository_path, expected in ARTIFACTS:
        checked_artifact(output, relative, repository_path, expected, args.offline)
    compiler = libraries / 'kotlin-compiler-embeddable-2.3.0.jar'
    if not compiler.is_file() or hashlib.sha256(compiler.read_bytes()).hexdigest() != \
            '8dbd882fa58c3d17e683a27390288315f8b490f8e3e3b1be4dc3e280d37e285a':
        raise ValueError('Gradle Kotlin 2.3.0 compiler checksum does not match the tested host toolchain')
    target = output / 'lib/kotlin-compiler.jar'
    target.unlink(missing_ok=True)
    # Kotlin's runtime packager canonicalizes this path and expects this name.
    # A symlink to the embeddable JAR resolves to an unrecognized compiler home.
    shutil.copyfile(compiler, target)
    for name in ('kotlin-stdlib', 'kotlin-reflect', 'kotlin-script-runtime'):
        source = libraries / (name + '-2.3.0.jar')
        if not source.is_file():
            raise ValueError('Gradle no longer supplies the expected Kotlin 2.3.0 host runtime: ' + name)
        destination = output / 'lib' / (name + '.jar')
        destination.unlink(missing_ok=True)
        destination.symlink_to(source)
    classpath = str(output / 'lib/kotlin-compiler.jar') + ':' + str(libraries / '*')
    launcher = output / 'kotlinc'
    launcher.write_text('#!/usr/bin/env bash\nset -euo pipefail\n'
        'exec "${JAVA_HOME:+${JAVA_HOME}/bin/}java" -Dkotlin.home=' + shlex.quote(str(output)) +
        ' -cp ' + shlex.quote(classpath) + ' org.jetbrains.kotlin.cli.jvm.K2JVMCompiler -kotlin-home ' +
        shlex.quote(str(output)) + ' "$@"\n')
    launcher.chmod(0o755)
    print('Prepared pinned Kotlin 2.3.0 module host tests')


if __name__ == '__main__':
    main()
