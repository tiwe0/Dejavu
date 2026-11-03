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

The inline provider is built from the checksum-pinned Dobby source at commit
`5dfc8546954ce3b3198132ab13fddb89ee92cdd7` (Apache-2.0). No prebuilt Prefab
archive is linked: it would bypass the maintained native-hook safety patch.

`patches/dobby-native-publication.patch` adds `DobbyHookWithPrepare`, which
publishes a valid original trampoline before activating an aligned, atomic
4-byte ARM64 branch. Its near stub uses a fresh anonymous mapping only: no
code-cave scanning, existing-text writes, or `MAP_FIXED` allocation. A missing
near mapping fails before activation instead of falling back to a multiword
entry patch. The regular `DobbyHook` API retains its routing for LSPlant and
publishes its output trampoline before activation.

The patch also preserves each patched page's current protection, propagates
write/restoration failures. Only activations that published branch bytes remain
registered for retry via `DobbyIsHooked` / `DobbyDestroy`; failures before writing
remove their reservation and clear the ordinary hook output. Internally CodePatch
returns -1 before writing and -2 after writing if protection restoration fails. Prepared callbacks must only
publish preallocated state (no allocation, logging, Dobby calls or exceptions).
Once a callback has run, its state and replacement must remain alive even when
activation returns an error, until the caller has safely retired the hook.
Dobby's exposed trampoline allocations are intentionally retained for the
process lifetime. Source-only Android build fixes are included for this pinned
revision (ELF assembly relocations and platform header/field consistency).

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

## KCP

`kcp/` vendors `ikcp.c` / `ikcp.h` from
<https://github.com/skywind3000/kcp> under the MIT license. It backs the
framework stream service's `KCP` transport; the host viewer compiles the same
source for local decoding. See `kcp/LICENSE` and `kcp/README.md`.
