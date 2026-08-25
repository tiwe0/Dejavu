# Dejavu Zygisk module

This directory contains the Android 15 arm64 Zygisk integration for Dejavu.
The runtime is split into a stable Loader and a replaceable per-process Agent so
normal development does not require restarting Zygote.

## Runtime architecture

- `zygisk/arm64-v8a.so` is the small Loader registered with Zygisk. It reads the
  exact process allowlist, loads the Agent from the module directory and creates
  a per-process live-control endpoint.
- `lib/arm64-v8a/libdejavu_agent.so` contains LSPlant, Dobby, the ART ELF
  resolver, HookManager, Lua and TinyCC. It is loaded only in selected app
  processes.
- `lib/arm64-v8a/libdejavu.so` is the locally built Dejavu runtime loaded by the
  Agent.
- The root companion monitors `run/<process>.lua` and opens two random abstract
  Unix domain sockets: one persistent Agent channel and one CLI listener. The
  Loader receives the private Agent endpoint through the short-lived Zygisk
  companion connection. Root-only CLI metadata is written to
  `run/<process>.<pid>.rpc`.

The companion verifies the Agent's kernel-reported PID and random 256-bit token.
CLI access also requires its separate token and root access to the metadata.
The sockets are local, do not create filesystem socket nodes and do not require
the target app to hold `android.permission.INTERNET`. Lua evaluation is
serialized by HookManager. Installed TinyCC native callbacks do not enter Lua
and can execute concurrently on different Java threads.

## Target selection

`config/targets.txt` contains exact process names. Empty files select no apps.

```text
bin.mt.plus
bin.mt.plus:worker
```

Only selected processes retain the Loader and load the Agent. Other app
processes request `DLCLOSE_MODULE_LIBRARY` after target selection.

## Build and package

The build targets Android API 35 and `arm64-v8a`. Both Loader and Agent are ELF64
AArch64 libraries with 16 KB `PT_LOAD` alignment and restricted exports.

```sh
make android-arm64
make -C zygisk test
make -C zygisk package
```

The installable module is written to:

```text
zygisk/dist/dejavu-zygisk-v0.1.0-arm64.zip
```

The first install and every Loader update require Zygisk to rescan modules and
restart Zygote. Agent-only updates do not.

## Agent development loop

Build, atomically replace the Agent, force-stop the target process and launch it
again:

```sh
ADB_SERIAL=c44d68aa make -C zygisk deploy-agent
```

`scripts/deploy-agent.sh` does not replace the Loader and does not restart
Zygote. Existing target processes continue using their already mapped Agent;
new target processes load the replacement.

Execute Lua management code through the request/response RPC channel:

```sh
ADB_SERIAL=c44d68aa \
  ./zygisk/scripts/dejavuctl -e 'return 6 * 7' -p bin.mt.plus

ADB_SERIAL=c44d68aa \
  ./zygisk/scripts/dejavuctl \
  zygisk/tests/device/live-control-smoke.lua \
  bin.mt.plus
```

`dejavuctl` discovers the running PID, reads its root-only endpoint metadata,
creates a temporary `adb forward` to the abstract socket, authenticates, sends
one RPC request and removes the forward. A successful Lua scalar return value
is printed to stdout. Lua errors, unavailable Agents and protocol failures are
printed to stderr with a non-zero exit status. Supported return values are nil,
boolean, integer, number and string. Concurrent local CLI invocations queue on
a host-side advisory lock because the Lua management session is intentionally
serial; TinyCC native callbacks remain unaffected and concurrent.

The Agent reconnects with exponential backoff from 100 ms to 5 seconds if its
channel is dropped while the same companion handler and abstract listener are
still alive. Lua state and installed hooks are retained. A full companion or
Zygisk restart creates a new endpoint and therefore still requires restarting
the target process.

Exercise and verify the same-PID reconnect path without changing Lua or hook
state:

```sh
ADB_SERIAL=c44d68aa \
  ./zygisk/scripts/dejavuctl --reconnect -p bin.mt.plus
```

The original file push remains available as a compatibility path:

```sh
ADB_SERIAL=c44d68aa \
  ./zygisk/scripts/push-control.sh \
  zygisk/tests/device/live-control-smoke.lua \
  bin.mt.plus
```

Successful file delivery produces both the script output and a companion
acknowledgement in logcat:

```text
DejavuHook: LIVE_CONTROL_SMOKE
DejavuZygisk: file control applied: request=1
```

## Lua management and native hooks

`config/init.lua` runs once after `Application.attach(Context)`, when the app
class loader is available. Lua only installs, lists, disables and clears hooks.
Hook implementations are TinyCC-compiled native callbacks.

```lua
local source = [[
extern void *dejavu_hook_call_original(
    void *env, void *backup, void *args, int is_static);
extern void dejavu_hook_log(const char *message);

void *dejavu_hook_callback(
    void *env,
    unsigned long long hook_id,
    void *backup,
    void *args,
    int is_static) {
    (void)hook_id;
    dejavu_hook_log("Activity.onResume");
    return dejavu_hook_call_original(env, backup, args, is_static);
}
]]

local id = hook.install(
    "android.app.Activity",
    "onResume",
    "()V",
    source)
hook.log(hook.list())
-- hook.remove(id) logically disables one hook.
-- hook.clear() logically disables every user hook.
```

Native callback code must be thread-safe. Generated code executes with the
target app's privileges and can crash or block that process, so keep the target
allowlist explicit.
