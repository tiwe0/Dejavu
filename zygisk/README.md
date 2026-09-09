# Dejavu Zygisk module

This directory contains the Android 15 `arm64-v8a` Zygisk integration for
Dejavu. It is a targeted dynamic-debugging bridge: Lua manages hooks, while
hook callbacks are compiled by TinyCC and run as native code in the target
process.

For the end-to-end installation and usage walkthrough, see
[docs/user-guide.md](docs/user-guide.md). The ABI reference remains in
[docs/hook-c-api.md](docs/hook-c-api.md).

The runtime is split into a small Loader and a replaceable per-process Agent.
Agent changes can be tested by restarting only the target app; a Zygote
restart is needed only when the Loader or module installation changes.

## Requirements

- Android 15 or newer (API 35+).
- A rooted `arm64-v8a` device with Magisk 27.0+ and Zygisk enabled.
- A host with `adb`, a C/C++ toolchain, Python 3 and an Android NDK.
- The Dejavu library built from the project root before packaging.

The current device test target is MT Manager (`bin.mt.plus`). The target is
selected by process name, not by package name alone.

APatch users can install the same module through APatch Manager. APatch's APM
supports the KernelSU-compatible module WebUI, but APatch itself does not
provide Zygisk; a compatible Zygisk provider (for example ZygiskNext) is still
required for the Loader and Agent.

## Architecture

- `zygisk/arm64-v8a.so` is the Loader registered with Zygisk. It reads the
  exact process allowlist and loads the Agent only in selected processes.
- `lib/arm64-v8a/libdejavu_agent.so` contains LSPlant, Dobby, the ART ELF
  resolver, HookManager, Lua and TinyCC.
- `lib/arm64-v8a/libdejavu.so` is the locally built Dejavu runtime loaded by
  the Agent.
- The root companion creates two random abstract Unix domain sockets per
  process: a persistent Agent channel and a CLI listener. Endpoint metadata is
  stored in `run/<process>.<pid>.rpc` and is readable only through root.

The sockets do not create filesystem nodes and do not require the target app to
hold `android.permission.INTERNET`. Lua management requests are serialized.
Installed TinyCC callbacks do not enter Lua and may run concurrently on
different Java threads.

## Select target processes

Edit `config/targets.txt` before packaging. Use one exact Android process name
per line; comments and blank lines are allowed. An empty file selects nothing.

```text
bin.mt.plus
bin.mt.plus:worker
```

Only selected processes retain the Loader and load the Agent. Other processes
request `DLCLOSE_MODULE_LIBRARY` after target selection.

## Build and install

Run these commands from the repository root:

```sh
# Build libdejavu.so used by the Agent.
make android-arm64

# Run host tests and compile/package the Zygisk module.
make -C zygisk test
make -C zygisk package
```

`make -C zygisk package` fetches pinned LSPlant/Dobby sources when needed and
produces:

```text
zygisk/dist/dejavu-zygisk-v0.1.0-arm64.zip
```

For example, copy it to the connected device with:

```sh
adb push zygisk/dist/dejavu-zygisk-v0.1.0-arm64.zip /sdcard/Download/
```

Install the ZIP from Magisk or APatch Manager, then reboot so Zygisk
rescans the module and restarts Zygote. The module installer rejects non-arm64
devices, Android versions below API 35 and Magisk versions below 27.0.

The package contains `webroot/index.html`. In APatch Manager, open the module's
WebUI entry to configure scope without editing files. The page uses the APM
root bridge to list installed packages and atomically update
`config/targets.txt`. Magisk users can use the same file, `dejavuctl` or
`push-control.sh`; Magisk Manager does not provide the APM WebUI entry.

After installation, confirm that `bin.mt.plus` is present in
`config/targets.txt`, launch MT Manager, and inspect the logs:

```sh
adb logcat -s DejavuZygisk:V DejavuAgent:V DejavuHook:V
```

Useful startup messages include `agent active`, `persistent Lua control
initialized` and `abstract-socket RPC control connected`.

## Agent-only development loop

The Agent and `config/init.lua` can be updated without replacing the Loader or
restarting Zygote:

```sh
ADB_SERIAL=c44d68aa make -C zygisk deploy-agent
```

`deploy-agent.sh` atomically copies the Agent and init script into the module,
then force-stops and starts the configured process. Override the defaults when
testing another process or activity:

```sh
ADB_SERIAL=c44d68aa \
PROCESS=bin.mt.plus:worker \
ACTIVITY=bin.mt.plus/.MainLightIcon \
make -C zygisk deploy-agent
```

An already running process keeps its mapped Agent until it is restarted. A
Loader change, module reinstall, companion restart or Zygisk restart still
requires a target-process restart after the Zygote restart.

### Production size and symbols

The build outputs in `out/` keep DWARF sections for native debugging. The
packaging step runs `llvm-strip --strip-debug` on all three runtime libraries,
while leaving the unstripped files in `out/`. A typical package drops from
about 3.8 MB compressed (16.3 MB uncompressed) to roughly 0.7 MB compressed
(2.1 MB uncompressed). Set `LLVM_STRIP` when the NDK tool is not on `PATH`:

```sh
LLVM_STRIP="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip" \
make -C zygisk package
```

## `dejavuctl` CLI

`scripts/dejavuctl` sends one Lua request over the authenticated local RPC
channel. It discovers the target PID and endpoint, creates a temporary `adb
forward`, authenticates, sends the request, prints the result, and removes the
forward.

Inline Lua:

```sh
ADB_SERIAL=c44d68aa \
  ./zygisk/scripts/dejavuctl -p bin.mt.plus -e 'return 6 * 7'
# 42
```

Lua file (the process may also be supplied as the second positional argument):

```sh
ADB_SERIAL=c44d68aa \
  ./zygisk/scripts/dejavuctl \
  zygisk/tests/device/live-control-smoke.lua bin.mt.plus
```

Useful options:

```text
-p, --process PROCESS   target process (default: bin.mt.plus)
-s, --serial SERIAL     adb serial (or set ADB_SERIAL)
--wait SECONDS          wait for PID/Agent metadata (default: 5)
--timeout SECONDS       socket operation timeout (default: 10)
--reconnect             reconnect and verify the same-PID Agent channel
```

`dejavuctl` supports scalar Lua results: `nil`, boolean, integer, number and
string. Errors, unavailable Agents and protocol failures go to stderr and
return a non-zero status. Local CLI invocations are serialized by a host-side
advisory lock because Lua state is intentionally single-threaded.

The Agent reconnects with exponential backoff from 100 ms to 5 seconds when
only its channel is dropped. Lua state and installed hooks survive that event:

```sh
ADB_SERIAL=c44d68aa \
  ./zygisk/scripts/dejavuctl --reconnect -p bin.mt.plus
```

The legacy file-control path is still available for compatibility. It watches
`run/<process>.lua` and is useful when diagnosing the RPC path:

```sh
ADB_SERIAL=c44d68aa \
  ./zygisk/scripts/push-control.sh \
  zygisk/tests/device/live-control-smoke.lua bin.mt.plus
```

## Lua hook management

`config/init.lua` runs once after `Application.attach(Context)`, when the app
class loader is available. Runtime control scripts sent by `dejavuctl` run in
the same management session. Lua only manages hooks; the callback implementation
is native C compiled in-process by TinyCC.

### API

```lua
local id = hook.install(class_name, method_name, jni_signature, c_source)
local text = hook.list()
local disabled = hook.remove(id)
local uninstalled = hook.uninstall(id)
local count = hook.clear()
hook.log("message")
```

- `hook.install` returns an integer hook ID. The source must define
  `before_hook`, `after_hook`, or both.
- `hook.list()` takes no arguments and returns one line per user hook:
  `id<TAB>active|disabled<TAB>Class#method(signature)`.
- `hook.remove(id)` performs a fast logical disable. It returns `true` only
  when an active hook was changed.
- `hook.uninstall(id)` performs physical cleanup: disables the hook, restores
  the target method through LSPlant, drains callbacks already in flight and
  releases compiled-module resources. It returns `false` for an unknown,
  already-uninstalled or internal bootstrap hook.
- `hook.clear()` logically disables all user hooks and returns the number that
  changed. It does not physically uninstall them.
- `hook.log(message)` writes to the `DejavuHook` logcat tag.

The WebUI represents each selected package by its default process name, which
is normally the package name. Apps that declare additional processes need
those exact names added manually to `config/targets.txt`, for example
`bin.mt.plus:worker`.

Example management script:

```lua
local source = [[
int before_hook(dejavu_hook_context *context) {
    int value = 0;
    DEJAVU_HOOK_TRY(dejavu_hook_get_arg_int(context, 0, &value));
    return dejavu_hook_set_arg_int(context, 0, value + 1);
}

int after_hook(dejavu_hook_context *context) {
    int result = 0;
    DEJAVU_HOOK_TRY(dejavu_hook_get_result_int(context, &result));
    return dejavu_hook_set_result_int(context, result + 1);
}
]]

local id = hook.install(
    "com.example.Target",
    "compute",
    "(I)I",
    source)
hook.log("installed hook " .. id)
hook.log(hook.list())
```

The full ABI, helper list and lifetime rules are in
[docs/hook-c-api.md](docs/hook-c-api.md).

## Native Hook C contract

The injected ABI preamble declares:

```c
int before_hook(dejavu_hook_context *context);
int after_hook(dejavu_hook_context *context);
```

Return `DEJAVU_HOOK_OK` (zero) on success. A non-zero return or failed helper
marks that phase as failed. The dispatch order is:

1. Dejavu copies the bridge argument array.
2. `before_hook` reads or changes the copy.
3. Unless a result was set in `before_hook`, the original Java method runs.
4. If the original did not throw, `after_hook` runs.
5. The final result is returned to LSPlant.

Setting a result in `before_hook` is an early return and skips the original
method. If `before_hook` fails, argument replacements are discarded and the
original is called with its original arguments; `after_hook` is skipped. If
`after_hook` fails, the result from before `after_hook` is preserved. These
rollbacks cannot undo mutations made through an object reference.

Use the channel matching the JNI descriptor:

| Java descriptor | C helpers |
| --- | --- |
| `Z B C S I J` | `*_arg_i64` / typed wrappers such as `*_arg_int` |
| `F D` | `*_arg_f64` / typed wrappers |
| objects and arrays | `*_arg_object` |
| return value | matching `*_result_*` helper |

String MUTF-8, `instanceof`, and controlled synchronous Java calls are exposed
as explicit helpers. Object references returned by the ABI are borrowed JNI
local references valid only during the current callback; never retain or pass
them to another thread. Generated C has no direct `JNIEnv` access.

Native callbacks are trusted code. They can crash or block the target process,
and a native fault cannot be converted into a Lua error. Keep callbacks short,
check helper return codes (use `DEJAVU_HOOK_TRY`), and keep the target allowlist
explicit.

## Concurrency and lifetime

- Lua management and CLI requests are serialized.
- Native callbacks can execute concurrently on different Java threads.
- Hook dispatch reads copy-on-write hook snapshots; updates publish a new
  snapshot and wait for in-flight callbacks before physical uninstall.
- The current implementation uses this small snapshot/drain scheme, not a
  general-purpose RCU implementation.
- Recursive calls on the same thread bypass Dejavu callbacks and invoke the
  original method.
- `remove`/`clear` are logical state changes; use `uninstall` when physical
  LSPlant and TinyCC cleanup is required.

## Verification

Run host tests and syntax checks after source or documentation changes:

```sh
make -C zygisk test
git diff --check
```

For an Android build with optional device smoke/stress bridge code:

```sh
RUN_DEVICE_SMOKE=1 \
RUN_DEVICE_STRESS=1 \
./zygisk/scripts/build-lsplant-android-arm64.sh
```

The production MT Manager smoke path is:

```sh
ADB_SERIAL=c44d68aa make -C zygisk deploy-agent
ADB_SERIAL=c44d68aa \
  ./zygisk/scripts/dejavuctl -p bin.mt.plus \
  zygisk/tests/device/mt-manager-stress.lua
```

Inspect `DejavuAgent`, `DejavuZygisk` and `DejavuHook` tags when a test fails.
The stress script covers concurrent dispatch, helper calls, logical removal,
physical uninstall and repeated uninstall behavior.

## Troubleshooting

**`target process is not running`**

Launch MT Manager and verify the exact process name with `adb shell pidof`.
Ensure that the same name appears in `config/targets.txt`, then restart the
process.

**`RPC endpoint is not ready`**

The Agent has not reached `Application.attach` yet, or the target was started
before the latest Agent was deployed. Wait briefly, relaunch the target, and
check `DejavuAgent` logcat output.

**Module installs but no Agent logs appear**

Check arm64/API/Magisk requirements, enable Zygisk, confirm the target list,
and reboot after the first install or any Loader change.

**Lua or hook compilation fails**

Run `dejavuctl` without shell quoting changes, confirm the JNI descriptor, and
read the returned error on stderr. Hook source must define at least one of
`before_hook` and `after_hook` and must compile as freestanding C against the
ABI preamble.

**A callback crashes the app**

Temporarily run `hook.remove(id)` or `hook.clear()` through `dejavuctl`, then
inspect `logcat` for the failing native callback. Rebuild with a smaller hook
and re-enable it only after validating argument types and reference lifetimes.
