#!/usr/bin/env python3
"""Compile actual patched Dobby paths with deterministic host-side OS/routing seams."""
import os
import shlex
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
        (temporary / 'android').mkdir()
        (temporary / 'android/log.h').write_text('''#pragma once
#define ANDROID_LOG_INFO 4
extern "C" int __android_log_print(int, const char *, const char *, ...);
''')
        original = (DOBBY / 'source/dobby.cpp').read_text()
        diagnostics = temporary / 'diagnostics.cpp'
        diagnostics.write_text('#include "dobby/dejavu_hook_diagnostics.h"\n' +
                               original[original.index('namespace dejavu_dobby {'):
                                        original.index('__attribute__((constructor))')])
        diagnostics_probe = temporary / 'diagnostics-probe.cpp'
        diagnostics_probe.write_text('''#include "dobby/dejavu_hook_diagnostics.h"
#include "dobby/dejavu_hook_mutation.h"
bool TestReadDobbyMutation() { return dejavu_dobby::hook_mutation_in_progress; }
const char *TestReadDobbyStage() { return dejavu_dobby::LastHookFailureStage(); }
void TestSetDobbyStage(const char *stage) { dejavu_dobby::SetHookFailureStage(stage); }
''')
        publication = temporary / 'publication.cpp'
        publication.write_text((TESTS / 'dobby_publication_test.cpp').read_text() + '\n' +
                               function(original, 'PUBLIC int DobbyIsHooked(') + function(original, 'PUBLIC int DobbyDestroy('))
        instrument = (DOBBY / 'source/InterceptRouting/InstrumentRouting.h').read_text()
        (temporary / 'dobby-instrument.h').write_text(function(instrument, 'PUBLIC inline int DobbyInstrument('))
        (temporary / 'dobby-lifecycle.h').write_text(function(original, 'PUBLIC int DobbyIsHooked(') +
                                                   function(original, 'PUBLIC int DobbyDestroy('))
        routing = (DOBBY / 'source/InterceptRouting/InterceptRouting.h').read_text()
        (temporary / 'dobby-active.h').write_text(function(routing, '  virtual void Active('))
        near = (DOBBY / 'source/InterceptRouting/NearBranchTrampoline/near_trampoline_arm64.cc').read_text()
        helpers = temporary / 'safe-near-helpers.h'
        helpers.write_text(function(near, 'static bool SafeBranchReachable(') +
                           function(near, 'static void *AllocateSafeNearPage('))
        pool_routing = temporary / 'safe-near-routing.h'
        pool_routing.write_text(function(near, 'Trampoline *GenerateSafeNearTrampolineBuffer(') +
                                function(near, 'bool PublishSafeNearTrampoline(') +
                                function(near, 'void ReleaseSafeNearTrampoline('))
        destructor = temporary / 'safe-near-destructor.h'
        destructor.write_text(function(routing, '  ~InterceptRouting()').replace('~InterceptRouting()', '~TestRouting()'))
        (temporary / 'MemoryAllocator').mkdir()
        (temporary / 'MemoryAllocator/CodeMemBuffer.h').write_text('#pragma once\n#include "MemoryAllocator/MemoryAllocator.h"\nusing CodeMemBlock = MemBlock;\n')
        (temporary / 'Interceptor.h').write_text((DOBBY / 'source/Interceptor.h').read_text())
        common = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                  '-I' + str(temporary), '-I' + str(TESTS / 'dobby_stubs'), '-I' + str(DOBBY / 'include'),
                  '-I' + str(DOBBY / 'source'), *shlex.split(os.environ.get('DOBBY_TEST_CXXFLAGS', ''))]
        for source, name, flags in (
            (publication, 'publication', []),
            (TESTS / 'dobby_near_page_test.cpp', 'safe-near-page',
             ['-DDOBBY_SAFE_NEAR_HELPERS="' + str(helpers) + '"']),
            (TESTS / 'dobby_near_pool_test.cpp', 'safe-near-pool',
             ['-DTARGET_ARCH_ARM64', '-DDOBBY_REAL_TRAMPOLINE="' + str(DOBBY / 'source/TrampolineBridge/Trampoline/Trampoline.h') + '"', '-DDOBBY_SAFE_NEAR_HELPERS="' + str(helpers) + '"',
              '-DDOBBY_SAFE_NEAR_POOL="' + str(DOBBY / 'source/InterceptRouting/NearBranchTrampoline/safe_near_page_pool.h') + '"',
              '-DDOBBY_SAFE_NEAR_ROUTING="' + str(pool_routing) + '"',
              '-DDOBBY_SAFE_NEAR_DESTRUCTOR="' + str(destructor) + '"']),
            (TESTS / 'dobby_mutation_test.cpp', 'mutation',
             ['-DTARGET_ARCH_ARM64', '-DDOBBY_REAL_TRAMPOLINE="' + str(DOBBY / 'source/TrampolineBridge/Trampoline/Trampoline.h') + '"',
              '-DDOBBY_SAFE_NEAR_HELPERS="' + str(helpers) + '"',
              '-DDOBBY_SAFE_NEAR_POOL="' + str(DOBBY / 'source/InterceptRouting/NearBranchTrampoline/safe_near_page_pool.h') + '"',
              '-DDOBBY_SAFE_NEAR_ROUTING="' + str(pool_routing) + '"',
              '-DDOBBY_SAFE_NEAR_DESTRUCTOR="' + str(destructor) + '"']),
            (TESTS / 'dobby_code_patch_test.cpp', 'code-patch',
             ['-DDOBBY_CODE_PATCH_SOURCE="' + str(DOBBY / 'source/Backend/UserMode/ExecMemory/code-patch-tool-posix.cc') + '"']),
            (TESTS / 'dobby_diagnostics_test.cpp', 'diagnostics', []),
            (TESTS / 'dobby_logging_test.cpp', 'logging-stdout',
             ['-I' + str(DOBBY / 'external/logging'), '-Wno-parentheses', '-Wno-unknown-pragmas', '-Wno-format',
              '-DDOBBY_LOGGING_SOURCE="' + str(DOBBY / 'external/logging/logging.cc') + '"']),
            (TESTS / 'dobby_logging_test.cpp', 'logging-android',
             ['-I' + str(DOBBY / 'external/logging'), '-Wno-parentheses', '-Wno-unknown-pragmas', '-Wno-format', '-DTEST_ANDROID_LOGGING',
              '-DDOBBY_LOGGING_SOURCE="' + str(DOBBY / 'external/logging/logging.cc') + '"']),
        ):
            executable = temporary / name
            subprocess.run([*common, *flags, str(source), str(diagnostics), str(diagnostics_probe),
                            '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True, timeout=15)
            print('OK: Dobby ' + name + ' safety contract')


if __name__ == '__main__':
    main()
