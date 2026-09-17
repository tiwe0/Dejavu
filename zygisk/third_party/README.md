# Third-party integration

## Zygisk

`include/zygisk.hpp` is pinned from the official Zygisk module sample at
commit `8ce26128f81baaed0b969aaf7f52f886b61af4ab` (API v5, 0BSD).

Source: <https://github.com/topjohnwu/zygisk-module-sample>

## LSPlant

The backend is pinned to LSPlant main commit
`d8b5d1dbb664abc606644036822e4bb64547edf6`, DexBuilder commit
`ac7fb2230954ee311808bad469b0db501f31bfb8`, and parallel-hashmap commit
`0cd57d29a959256ed66b2afdd1009928fc625d09`. Do not use floating Maven
versions. This LSPlant revision advertises Android 5-17 support and requires
CMake 3.28+, C++23 and Android NDK r29. LSPlant and DexBuilder are LGPL-3.0 and
require:

- a standalone/static C++ runtime suitable for a Zygisk process;
- an inline hook/unhook implementation;
- a `libart.so` resolver that reads both `.dynsym` and `.symtab`;
- flexible 16 KB page support.

The final static backend archive must export:

```c++
extern "C" DejavuLsplantInitResult dejavu_lsplant_backend_initialize(JNIEnv *env);
```

The inline provider is Dobby Prefab 1.2, pinned by SHA-256
`251f48ae21686d7f69276c50644ca345f450e45110057437f7d76bb14cddf3a1`.
Its corresponding public source is retained at Dobby commit
`5dfc8546954ce3b3198132ab13fddb89ee92cdd7`. Dobby is Apache-2.0.

`patches/lsplant-main-reflection-shorty.patch` replaces LSPlant's hard
dependency on the private ART `GetMethodShorty` symbol with its existing Java
reflection calculation. Some Android 15 vendor ART images strip that symbol.
The fallback only runs while installing a hook and is not part of the native
callback hot path.

The former Android 15 interpreter-bridge and JIT collection patches were
removed because their behavior is now implemented upstream.

Run `scripts/fetch-third-party.sh` to materialize these dependencies, then
`scripts/build-lsplant-android-arm64.sh`. The backend calls `lsplant::Init`
at most once per selected process. If that call is attempted, the Zygisk
library remains resident even when initialization fails.

Source: <https://github.com/LSPosed/LSPlant>
