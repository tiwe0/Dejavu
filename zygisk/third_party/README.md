# Third-party integration

## Zygisk

`include/zygisk.hpp` is pinned from the official Zygisk module sample at
commit `8ce26128f81baaed0b969aaf7f52f886b61af4ab` (API v5, 0BSD).

Source: <https://github.com/topjohnwu/zygisk-module-sample>

## LSPlant

The Android 15 backend is pinned to LSPlant v6.4 commit
`7217ac6f41e2bda549e4acb54e632abb03e5ccaf`, DexBuilder commit
`9d57844a301077abf4c29e061b2458c56a363c8f`, and parallel-hashmap commit
`65775fa09fecaa65d0b0022ab6bf091c0e509445`. Do not use floating Maven
versions. LSPlant and DexBuilder are LGPL-3.0 and require:

- a standalone/static C++ runtime suitable for a Zygisk process;
- an inline hook/unhook implementation;
- a `libart.so` resolver that reads both `.dynsym` and `.symtab`;
- flexible 16 KB page support.

The final static backend archive must export:

```c++
extern "C" bool dejavu_lsplant_backend_initialize(JNIEnv *env);
```

The inline provider is Dobby Prefab 1.2, pinned by SHA-256
`251f48ae21686d7f69276c50644ca345f450e45110057437f7d76bb14cddf3a1`.
Its corresponding public source is retained at Dobby commit
`5dfc8546954ce3b3198132ab13fddb89ee92cdd7`. Dobby is Apache-2.0.

`patches/lsplant-v6.4-reflection-shorty.patch` replaces LSPlant's hard
dependency on the private ART `GetMethodShorty` symbol with its existing Java
reflection calculation. Some Android 15 vendor ART images strip that symbol.
The fallback only runs while installing a hook and is not part of the native
callback hot path.

`patches/lsplant-v6.4-android15-interpreter-bridge.patch` backports LSPlant's
upstream Android 15 fallback for ART builds that strip
`ClassLinker::SetEntryPointsToInterpreter` and the quick trampoline symbols.
It resolves `Instrumentation::GetOptimizedCodeFor` and derives the interpreter
bridge from a cloned Java method during backend initialization.

`patches/lsplant-v6.4-jit-do-collection.patch` adds ART's newer
`JitCodeCache::DoCollection` as the fallback for the removed
`GarbageCollectCache` hook, matching upstream LSPlant commit `0d9faca3`.

Run `scripts/fetch-third-party.sh` to materialize these dependencies, then
`scripts/build-lsplant-android-arm64.sh`. The backend calls `lsplant::Init`
at most once per selected process. If that call is attempted, the Zygisk
library remains resident even when initialization fails.

Source: <https://github.com/LSPosed/LSPlant>
