#!/usr/bin/env python3
"""Compile actual patched Dobby paths with deterministic host-side OS/routing seams."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DOBBY = ROOT / 'third_party/Dobby'
TESTS = ROOT / 'tests'


def function(source, name):
    start = source.index(name)
    body = source.index('{', start)
    depth = 1
    cursor = body + 1
    while depth:
        depth += (source[cursor] == '{') - (source[cursor] == '}')
        cursor += 1
    return source[start:cursor] + '\n'


def main():
    compiler = os.environ.get('CXX', 'c++')
    with tempfile.TemporaryDirectory(prefix='dejavu-dobby-safety-') as temporary:
        temporary = Path(temporary)
        original = (DOBBY / 'source/dobby.cpp').read_text()
        publication = temporary / 'publication.cpp'
        publication.write_text((TESTS / 'dobby_publication_test.cpp').read_text() + '\n' +
                               function(original, 'PUBLIC int DobbyIsHooked(') + function(original, 'PUBLIC int DobbyDestroy('))
        routing = (DOBBY / 'source/InterceptRouting/InterceptRouting.h').read_text()
        (temporary / 'dobby-active.h').write_text(function(routing, '  virtual void Active('))
        near = (DOBBY / 'source/InterceptRouting/NearBranchTrampoline/near_trampoline_arm64.cc').read_text()
        helpers = temporary / 'safe-near-helpers.h'
        helpers.write_text(function(near, 'static bool SafeBranchReachable(') +
                           function(near, 'static void *AllocateSafeNearPage('))
        (temporary / 'Interceptor.h').write_text((DOBBY / 'source/Interceptor.h').read_text())
        common = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                  '-I' + str(temporary), '-I' + str(TESTS / 'dobby_stubs'), '-I' + str(DOBBY / 'include'),
                  '-I' + str(DOBBY / 'source')]
        for source, name, flags in (
            (publication, 'publication', []),
            (TESTS / 'dobby_near_page_test.cpp', 'safe-near-page',
             ['-DDOBBY_SAFE_NEAR_HELPERS="' + str(helpers) + '"']),
            (TESTS / 'dobby_code_patch_test.cpp', 'code-patch',
             ['-DDOBBY_CODE_PATCH_SOURCE="' + str(DOBBY / 'source/Backend/UserMode/ExecMemory/code-patch-tool-posix.cc') + '"']),
        ):
            executable = temporary / name
            subprocess.run([*common, *flags, str(source), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True, timeout=15)
            print('OK: Dobby ' + name + ' safety contract')


if __name__ == '__main__':
    main()
