# Dejavu

**English** | [简体中文](README.zh-CN.md)

`libdejavu.so` is an Android arm64 shared library skeleton embedding:

- Lua 5.4.8
- TinyCC 0.9.27-frida

The public ABI is declared in `include/dejavu.h`. Lua and TinyCC are pinned as
Git submodules and their symbols are hidden. The engine compiles freestanding C
directly into memory; Lua is a small control layer around that engine.

The core `libdejavu.so` library has no Magisk dependency. It can be embedded or
loaded by any application that controls its own process. The `zygisk/` directory
is an optional integration for injecting Dejavu into other Android processes;
that path requires a compatible Zygisk provider, not Magisk specifically.

For the Android Zygisk integration, common hook tasks also have a higher-level
pure-Lua helper layer, `hookx.*`; see `zygisk/docs/user-guide.md` and
`zygisk/docs/cookbook.md`.

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
is required.

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

Every successful push to `main` publishes the packaged Zygisk module as a
GitHub prerelease named `v<module-version>-build.<workflow-run>`. Pull requests
and `develop` builds never publish releases. Pushing a matching `v*` tag, such
as `v0.2.0`, publishes the corresponding stable release instead. A release is
created only after the host tests, Android API 26 baseline, compatibility
builds, Zygisk package, and code-quality checks all pass.

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
