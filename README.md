# Dejavu

**English** | [简体中文](README.zh-CN.md)

`libdejavu.so` is an Android arm64 shared library skeleton embedding:

- Lua 5.4.8
- TinyCC 0.9.27-frida

The public ABI is declared in `include/dejavu.h`. Lua and TinyCC are pinned as
Git submodules and their symbols are hidden. The engine compiles freestanding C
directly into memory; Lua is a small control layer around that engine.

## Disclaimer and authorized use

Dejavu is a **personal, self-use security research tool**. Use it only on
devices you own or on processes/apps you have explicit authorization to test.
You are responsible for complying with applicable laws, contracts and platform
policies (including the target app's terms of service); do not use it for
unauthorized access, bypassing protections, or causing harm to others.

The software is provided **"as is"**, without warranty of any kind. The authors
are not liable for any damage, data loss, account bans, legal consequences or
other harm arising from its use or misuse. You are solely responsible for your
actions and for ensuring that your testing is authorized.

The core `libdejavu.so` library has no Magisk dependency. It can be embedded or
loaded by any application that controls its own process. The `zygisk/` directory
is an optional integration for injecting Dejavu into other Android processes;
that path requires a compatible Zygisk provider, not Magisk specifically.

For the Android Zygisk integration, common hook tasks also have a higher-level
pure-Lua helper layer, `hookx.*`; see `zygisk/docs/user-guide.md` and
`zygisk/docs/cookbook.md`.

The Zygisk integration supports both Java/Kotlin method hooks (LSPlant) and
inline native function hooks (Dobby + a TinyCC-compiled replacement); see
`zygisk/docs/native-hook-api.md` for the `native.*` API and its limits.

Active invocation is available as well: `native.call(...)` calls an exported
native function through a TinyCC wrapper, and `java.invoke(...)` /
`java.new(...)` / `java.release(...)` call Java methods through JNI with a
signature-driven type conversion protocol; returned values (primitives,
strings and object/array handles) can be chained into subsequent calls (see
`zygisk/docs/java-invoke-api.md` and `zygisk/docs/native-hook-api.md`).

The control session also exposes agent helpers: `fs.read/write/exists/remove/
list/stat` for files in the target sandbox, and `module.load/unload/loaded/
symbols/resolve` for loading shared libraries, enumerating ELF symbols and
resolving hidden (`LOCAL`) symbols; `native.call`/`native.install` fall back
to the ELF symbol table for non-exported symbols. A native **plugin** mechanism
(`plugin.load/list/call/unload`, C ABI in `include/dejavu_plugin.h`, see
`zygisk/docs/plugin-api.md`) lets a shared object register Lua functions, use
host mem/module/hook services, and auto-load plugins from
`debug.dejavu.plugins` before app code runs. A trusted allowlist
(`debug.dejavu.plugins.allowed` / `config/plugins.txt`) plus optional per-path
SHA-256 hashes and unload-safety flags bound what may load. Memory helpers
(`mem.read/write`, typed `read_u*/write_u*`, `mem.dump`, `mem.query`,
`mem.protect`, `mem.maps`) provide guarded access to the target address space.
See `zygisk/docs/agent-helpers.md`.

## Engine model

The first ABI deliberately supports one generated entry signature:

```c
int dejavu_entry(void);
```

Direct C usage:

```c
dejavu_engine *engine = dejavu_engine_create();
dejavu_module *module = NULL;
char error[512];
int result;

dejavu_engine_compile(
    engine,
    "int dejavu_entry(void) { return 6 * 7; }",
    &module,
    error,
    sizeof(error));
dejavu_module_call(module, &result);
dejavu_module_destroy(module);
dejavu_engine_destroy(engine);
```

Generated code is built with `-nostdlib`. Host functions must be explicitly
registered with `dejavu_engine_add_symbol()` before compilation; unresolved
symbols are rejected instead of being resolved from the process with
`dlsym()`.

This allowlist limits linking; it is not a security sandbox. Generated native
code still executes with the permissions and address space of the host process.

The Lua control surface exposes only `dejavu.compile()` plus the base, table,
string, and math libraries:

```lua
local run = dejavu.compile [[
  int dejavu_entry(void) { return 6 * 7; }
]]
return run()
```

## Build smoke

Requirements: a Linux or macOS host and an Android NDK. No CMake installation
is required. The build scripts compile a patched copy of the pinned TinyCC
sources (`patches/tinycc-runtime-safety.patch`, which keeps writable data off
executable pages and turns relocation failures into errors); the submodule
itself is not modified.

```sh
git submodule update --init --recursive
export ANDROID_NDK_HOME="$HOME/Android/Sdk/ndk/android-ndk-r29"
make smoke
```

`make smoke` first runs the engine and Lua control tests on the host, then
cross-compiles and inspects the Android artifact. Use `make android-arm64` when
only the Android build is needed.

The default build uses `arm64-v8a` and native API level 26, producing:

```text
out/android-arm64-v8a-api26/libdejavu.so
```

Override the native API level or build type when needed:

```sh
ANDROID_API=35 BUILD_TYPE=release make smoke
```

For an Android app, `targetSdkVersion` is configured in the app manifest or
Gradle build. The NDK API selected here is the minimum Android version whose
native APIs the `.so` may use. The API 26 artifact targets Android 8 and newer
while using stable libc and NDK APIs. The complete Zygisk integration currently
supports Android 8 through 15 (API 26-35); Android 16 requires separate ART and
LSPlant compatibility validation.

### Android compatibility status

| Android versions | API levels | Status |
| --- | --- | --- |
| Android 8-14 | 26-34 | Build and installer support; runtime acceptance must still be verified on each device/ROM generation |
| Android 15 | 35 | Build and installer support plus the current MT Manager device/stress validation path |
| Android 16-17 | 36-37 | Supported by upstream LSPlant; Dejavu runtime acceptance is pending, so the module installer does not enable these versions yet |

### Automated releases

Pushes to `main` and pull requests run the full build and test pipeline. A
release is published when a version is explicitly introduced:

- bump the `version=` line in `zygisk/module/module.prop` and push to `main`:
  CI detects the change and publishes the stable `vX.Y.Z` release after all
  checks pass (the tag is created from that commit); or
- push a matching `v*` tag, such as `v0.5.0`, to create the stable release; or
- run the workflow manually with `workflow_dispatch` to create a prerelease
  named `v<module-version>-build.<workflow-run>` from the selected branch.

A release is created only after the host tests, Android API 26 baseline,
compatibility builds, Zygisk package, and code-quality checks all pass. A push
that does not change the module version only builds artifacts and never
publishes a release.

The build smoke checks that the output is an ELF64 AArch64 shared object, has
the expected SONAME, uses 16 KB-compatible load segment alignment, and exports
exactly the Dejavu ABI. Runtime smoke entry points are included for device
testing:

- `dejavu_smoke_lua()` uses Lua to compile and invoke a C entry function.
- `dejavu_smoke_tcc()` compiles and executes an in-memory AArch64 function.
- `dejavu_smoke()` runs both checks.

Executable memory is allocated from a dedicated, page-aligned mapping. TinyCC
writes and relocates through RW pages and changes generated code pages to RX;
it never changes allocator heap pages to executable. Device testing is still
required to validate the target process SELinux policy and page-size
configuration on every supported Android generation.

## Unified CLI

`./dejavu` wraps the build scripts, module packaging and the `dejavuctl`
control channel into one command:

```sh
./dejavu status                                 # host/device/module state
./dejavu build [--api 26]                       # build libdejavu.so
./dejavu package [--api 26]                     # build and package the module zip
./dejavu install [--zip PATH] [--soft-reboot]   # push + install via ksud/magisk
./dejavu targets list|add|remove|set|clear      # edit device targets.txt
./dejavu deploy-agent -p <process>              # hot-deploy agent + init.lua
./dejavu exec -p <process> -e 'return hook.list()'
./dejavu exec -p <process> hooks/demo.lua
./dejavu native -p <process> list|stats|uninstall|clear
./dejavu native -p <process> install libc.so read replacement.c
./dejavu native -p <process> call libc.so getpid call.c
./dejavu native -p <process> install-file libc.so read /data/.../hook.c
./dejavu native -p <process> call-file libc.so getpid /data/.../call.c
./dejavu java -p <process> invoke java.lang.String valueOf '(I)Ljava/lang/String;' --arg 42
./dejavu java -p <process> new java.lang.StringBuilder '(Ljava/lang/String;)V' --arg abc
./dejavu java -p <process> release <handle>
./dejavu plugin -p <process> list
./dejavu plugin -p <process> allow <device-plugin.so>   # trusted allowlist
./dejavu plugin -p <process> load <device-plugin.so>
./dejavu plugin -p <process> jit <device-source.c>   # TinyCC JIT
./dejavu plugin -p <process> call <name> <namespace.function> [--arg VALUE]
./dejavu plugin -p <process> unload <name>
./dejavu plugin -p <process> skill <name>   # read per-plugin SKILL.md


./dejavu repl -p <process>
./dejavu watch hooks/demo.lua -p <process>
./dejavu log [-p <process>] [--dump] [--clear]
./dejavu test -p <process> [--with-hang-test]
```

It honors `ADB`, `ADB_SERIAL` and `-s/--serial`, and falls back to streaming
files over the adb shell when the adb binary cannot read host paths (for
example the Windows `adb.exe` invoked from WSL). Run `./dejavu --help` or
`./dejavu <command> --help` for the full option list.

Agent/assistant usage is documented in the companion [SKILL.md](SKILL.md).

## Versions

- `dejavu_version()` / `DEJAVU_VERSION_*` in `include/dejavu.h` identify the
  **core library ABI version** (currently `0.1.0`). Bump it when the public C
  ABI changes.
- `zygisk/module/module.prop` and release tags (`vX.Y.Z`) identify the
  **Zygisk module package version** (currently `0.5.0`). Module packaging,
  loader/agent behavior and documentation changes bump it independently.
- CI builds and tests pushes to `main`; a version bump in `module.prop`
  publishes the stable release, a matching `v*` tag publishes the stable
  release, and a manual `workflow_dispatch` run publishes a prerelease.

## AI agent interface (MCP)

The CLI includes an MCP stdio server so an AI agent can control a target
process through tools instead of ad-hoc shell text:

```sh
./dejavu mcp -p com.android.settings
```

Example MCP client configuration:

```json
{
  "mcpServers": {
    "dejavu": {
      "command": "/absolute/path/to/Dejavu/dejavu",
      "args": ["mcp", "-p", "com.android.settings"]
    }
  }
}
```

The server exposes 20 tools covering status/state, measured targets, Lua exec,
plugin install/load/JIT/call/unload/skill/allow, native list/call, Java
invoke/new, log tail, the device acceptance suite, agent deployment, and a `dejavu_cli` escape hatch for the remaining CLI surface. Tool
results are MCP text content; `./dejavu mcp --help` and
[tests/mcp_server_test.py](tests/mcp_server_test.py) show the protocol surface.
For an authorized rooted device, run the real lifecycle check with:

```sh
ADB=/path/to/adb ADB_SERIAL=<serial> TARGET_PROCESS=com.android.settings \
  python3 tests/mcp_device_test.py
```

The agent's Lua session is persistent for the lifetime of the target process:
Lua globals, loaded plugins and hooks survive across tool calls. Use
`dejavu_state` before multi-step work and unload plugins when done. Plugin
SKILL.md text is exposed only on demand and remains untrusted documentation.

## Testing

- Host: `make host-smoke` runs the engine/Lua tests plus regressions for
  writable generated data, large callback argument lists and (on Linux)
  simulated execmem denial; `make android-arm64` validates the Android
  artifact.
- Zygisk: `make -C zygisk test` runs the host-side C/C++/Lua/CLI tests.
- Device: `TARGET_PROCESS=<package> ./zygisk/scripts/device-test.sh` automates
  the on-device acceptance flow against a rooted device with a Zygisk provider
  and this module installed (see `zygisk/README.md`). It uses the file control
  channel, never reboots the device, and verifies control, instance-method
  hooks, native function hooks (install and active call), Java active
  invocation (static/constructor/instance with type conversions), chained
  invocation results (including cross-API), filesystem and module/symbol
  helpers, hidden-symbol calls, memory read/write/dump/protection, writable
  generated data, oversized responses and header-include behavior; `--with-hang-test` additionally exercises the relay timeout.
- Patched dependencies: see `patches/README.md`.

## Known limitations

- Not a sandbox: device hook callbacks run with the target process privileges.
- Generated code cannot `#include` headers on device; only the fixed preamble
  API is available (see `zygisk/docs/hook-c-api.md`).
- Execmem-restricted ROMs may reject generated code. DejaVu reports an error
  instead of killing the process, but hooks still need per-device acceptance.
- A hung Lua control script can block that process's Agent. The companion
  channel times out instead of blocking forever, and restarting the target
  process recovers.
- LSPlant relies on private ART symbols; each Android generation needs device
  validation. Android 16/17 support remains pending.

## Examples (not framework capabilities)

`plugins/unpack/` is a **use-case demo**, not a stable framework feature. It is
built as a dynamically loaded plugin (`plugins/unpack/build.sh` →
`libdejavu_unpack.so`) and loaded through `plugin.load` /
`debug.dejavu.plugins`; the agent has no unpack code compiled in. The plugin
combines the generic hooks with a Lua strategy (manifest namespace →
dumped-DEX class parsing → paced active invocation) and is driven by
`plugins/unpack/cli.py`. See `plugins/unpack/README.md` and
`plugins/unpack/design.md`.

## License

Dejavu is released under the MIT License; see [LICENSE](LICENSE).
Third-party components and their licenses are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

The bundled third-party components keep their own licenses: Lua 5.4 (MIT),
TinyCC (LGPL-2.1), LSPlant and DexBuilder (LGPL-3.0), and Dobby (Apache-2.0).
The module package ships the project LICENSE; review the third-party licenses
before redistributing binaries.
