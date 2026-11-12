#!/usr/bin/env python3
"""Verify strip on copies of all five real Android runtime ELF artifacts."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    return hashlib.sha256(path.read_bytes()).digest()


def symbols(readelf, path):
    output = subprocess.check_output([str(readelf), '--dyn-syms', '--wide', str(path)], text=True)
    result = set()
    exported = set()
    for line in output.splitlines():
        fields = line.split()
        if len(fields) < 8 or not fields[0].rstrip(':').isdigit():
            continue
        name = fields[7]
        # Section indices may legitimately be renumbered after stripping.
        result.add((*fields[1:6], fields[6] != 'UND', name))
        if fields[6] != 'UND':
            exported.add(name.split('@', 1)[0])
    if not result:
        raise AssertionError('Missing dynamic symbol table: ' + str(path))
    return result, exported


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk', type=Path, default=os.environ.get('ANDROID_NDK_HOME'))
    parser.add_argument('--binary-dir', type=Path,
                        default=ROOT / 'zygisk/out/lsplant-android-arm64-v8a-api26')
    parser.add_argument('--core', type=Path, default=ROOT / 'out/android-arm64-v8a-api26/libdejavu.so')
    args = parser.parse_args()
    if args.ndk is None:
        parser.error('Set ANDROID_NDK_HOME or --ndk to verify the Android artifacts')
    candidates = sorted(args.ndk.glob('toolchains/llvm/prebuilt/*/bin/llvm-readelf'))
    if not candidates:
        parser.error('NDK llvm-readelf is missing')
    readelf = candidates[0]
    files = [
        (args.binary_dir / 'libdejavu_zygisk.so', {'zygisk_module_entry', 'zygisk_companion_entry'}),
        (args.binary_dir / 'libdejavu_agent.so', {'dejavu_agent_get_api'}),
        (args.core, {'dejavu_version', 'dejavu_engine_create', 'dejavu_engine_compile',
                     'dejavu_control_session_create', 'dejavu_control_session_register', 'dejavu_value_release'}),
        (args.binary_dir / 'dejavu-root-helper', set()),
        (args.binary_dir / 'dejavuerouter', set()),
    ]
    original = {path: digest(path) for path, _ in files}
    with tempfile.TemporaryDirectory(prefix='dejavu-runtime-strip-') as directory:
        copies = []
        before = []
        for path, required in files:
            target = Path(directory) / path.name
            shutil.copyfile(path, target)
            before.append(symbols(readelf, target)[0])
            copies.append(target)
        environment = dict(os.environ, ANDROID_NDK_HOME=str(args.ndk))
        environment.pop('LLVM_STRIP', None)
        subprocess.run(['bash', '-c', 'source "$1"; shift; strip_runtime_files "$@"',
                        'strip-test', str(ROOT / 'zygisk/scripts/strip-runtime.sh'), *map(str, copies)],
                       env=environment, check=True, timeout=60)
        for index, ((source, required), path) in enumerate(zip(files, copies)):
            output = subprocess.check_output([str(readelf), '--section-headers', '--wide', str(path)], text=True)
            sections = re.findall(r'^\s*\[\s*\d+\]\s+(\S+)', output, re.MULTILINE)
            assert '.dynsym' in sections, str(path) + ': .dynsym removed'
            assert '.symtab' not in sections, str(path) + ': .symtab retained'
            assert not any(name.startswith(('.debug', '.zdebug')) for name in sections), str(path) + ': debug section retained'
            after, exports = symbols(readelf, path)
            assert before[index] == after, str(path) + ': dynamic symbols changed'
            assert required <= exports, str(path) + ': required runtime exports missing'
            assert digest(source) == original[source], str(source) + ': original build output changed'
            print(f'PASS: {source.name}: {source.stat().st_size} -> {path.stat().st_size} bytes; '
                  'no debug/.symtab, dynamic symbols and original build preserved')


if __name__ == '__main__':
    main()
