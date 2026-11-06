# Native plugins

Dejavu can load native plugins at runtime instead of compiling every feature
into the agent. A plugin is an arm64 shared object exporting one C entry point:

```c
#include <dejavu_plugin.h>

extern "C" __attribute__((visibility("default")))
const DejavuPluginDescriptor *dejavu_plugin_entry(void);
```

The descriptor contains `abi_version`, `struct_size`, name/version, flags,
`initialize`, `on_control_ready`, `shutdown`, optional `capabilities` and
optional `on_unload_prepare`. All ABI structs are
append-only and must be validated with `struct_size`; v2 is the current frozen layout (the pre-freeze v1 descriptor had a different shape and is rejected).

`initialize` receives a per-instance `DejavuPluginHost` table with stable function
pointers and an opaque `host_context`. Save this instance's host table and pass
its context unchanged to host services, including from native worker or hook
callback threads. Contexts and stream handles are never shared between plugins.
An initializer that returns failure must stop and join any workers it started
before returning; the framework cancels its streams but cannot join unknown
plugin-owned threads or call shutdown on an uninitialized state.

- `register_function(host, namespace, name, function, context, error, size)`
  registers a Lua function. Namespaces must be simple identifiers and must not
  use reserved framework names (`hook`, `fs`, `module`, `native`, `java`,
  `mem`, `plugin`); duplicate namespace/function pairs are rejected.
- `log_message` writes to logcat under `DejavuPlugin`;
- `get_app_class_loader` returns the current app ClassLoader (or `NULL` before
  Lua control is initialized);
- `alloc` / `free` are agent-owned allocation helpers for plugin results;
- `get_interface(host, "mem"|"module"|"hook", version)` returns optional host
  services (see below).

The descriptor may append optional `capabilities` bits
(`DEJAVU_PLUGIN_CAP_MEM/MODULE/HOOK/FS/JAVA/STREAM`); `plugin.list()` prints them as
`caps=0x...`. Read the field only when `struct_size` reaches its offset.
Capabilities are enforced: `get_interface("mem"|"module"|"hook"|"fs")` and
the corresponding `dejavu_jit_*` helpers return failure unless the plugin
declared the matching bit. A plugin that resolves `libc.so` symbols and then
installs a hook needs `HOOK|MODULE`; declare every service actually used.

`on_control_ready` is called once the Lua control session exists; plugins that
need the control session/ClassLoader should defer that work there instead of
doing it in `initialize`.

`on_unload_prepare` (read only when `struct_size` reaches its offset) is called
after new callbacks are disabled and the plugin's streams are retired. Blocking
stream reads/accepts are cancelled before this callback, which must stop and
join the plugin's workers. The callback runs without the manager mutex. The
framework then removes remaining owned hooks; failed unpatch/protection cleanup
retains the module for a later unload retry. `shutdown` also runs without the
manager mutex and precedes module teardown. An executing registered callback
prevents its own plugin from being unloaded.

The ABI lives in [`include/dejavu_plugin.h`](../../include/dejavu_plugin.h) and
is versioned with `DEJAVU_PLUGIN_ABI_VERSION`. Plugins are built with
`-fvisibility=hidden`; only `dejavu_plugin_entry` is exported.

## Host services

Through `host->get_interface`:

- **mem** (`DEJAVU_MEM_INTERFACE_VERSION`): safe same-process
  `read`/`write` (via `process_vm_readv`) and `query` mapping bounds/protection
  (bit 1 read, 2 write, 4 execute).
- **module** (`DEJAVU_MODULE_INTERFACE_VERSION`): `resolve(library, symbol)`
  including non-dynamic `LOCAL` symbols (ELF symbol table).
- **fs** (`DEJAVU_FS_INTERFACE_VERSION`): `read_file`, `write_file`, `exists`,
  `file_size`, `remove_file` for app-accessible paths. JIT plugins use
  `dejavu_jit_fs_read/write/exists/size/remove`; file reads are capped at
  16 MiB and paths at 4096 bytes.
- **stream** (`DEJAVU_STREAM_INTERFACE_VERSION`, plus append-only
  `DEJAVU_STREAM_INTERFACE_VERSION_2`): v1 exposes `open(transport, endpoint)`,
  `write(handle, data, size)`, `close(handle)`. Transports are
  `DEJAVU_STREAM_TRANSPORT_TCP/UDP/KCP` (1/2/3); endpoint is `host:port`.
  Each write is one framed record; viewers receive the payload. Handles are
  owned by the plugin and closed automatically on unload. JIT plugins use
  `dejavu_jit_stream_open/write/close`; the `stream` capability bit must be
  declared (`DEJAVU_PLUGIN_CAP_STREAM`, 0x20).
  v2 adds device-side RPC servers: `listen(transport, endpoint, backlog)`,
  `accept(listener, timeout_ms)` and `read(handle, buffer, capacity,
  timeout_ms, &size_read)`. Request it with
  `host->get_interface("stream", 2)` (`DejavuStreamInterfaceV2`). `listen` is
  TCP-only; the endpoint is `host:port` (no transport prefix). Bind
  `127.0.0.1` for loopback (e.g. `adb forward`) or `0.0.0.0` for LAN peers
  (the target process needs the INTERNET permission). `read`
  returns exactly one record using the same framing as `write`: 1 on a record,
  0 on timeout, -1 on EOF/error/oversized record; `timeout_ms < 0` blocks.
  JIT equivalents are `dejavu_jit_stream_listen/accept/read`. The Lua control
  session exposes the same server surface through
  `hook.stream_listen/accept/read/write/close`.
  Native v1 and v2 callers use their saved `host_context` on every operation.
  JIT helpers use the current initializer/registered callback identity. A JIT
  worker must explicitly bind an opaque credential captured by its owner:

  ```c
  /* In initialize() or an authorized registered callback, before spawning. */
  void *credential = dejavu_jit_stream_capture_context();
  /* In that plugin's worker thread. */
  if (dejavu_jit_stream_bind_context(credential) != 0) return;
  /* open/listen/accept/read/write/close now use this worker's owner. */
  dejavu_jit_stream_bind_context(0); /* unbind before the worker returns */
  ```

  Capture fails without the stream capability. A known callback's identity
  always takes precedence over a worker binding, and credentials are revoked
  when their plugin begins unloading. Reloading creates a new identity; old
  credentials and handles cannot be reused. Binding grants only stream access.
- **hook** (`DEJAVU_HOOK_INTERFACE_VERSION`): `install(target, replacement,
  &original)` via Dobby and `uninstall(target)`. Hooks installed through this
  interface are owned by the calling plugin; duplicate targets and
  non-owner uninstalls are rejected. Plugins should quiesce native callbacks
  and remove their hooks during `on_unload_prepare`. The framework also retries
  remaining host-tracked hooks before shutdown; failed unpatch/protection
  restoration refuses teardown and keeps the module mapped for retry.

Interface structs also carry `version` and `struct_size`; extend by appending
fields and bumping the interface version.

## Lua control API

```lua
plugin.load('/data/user_de/0/<pkg>/files/libmy_plugin.so')
plugin.list()
plugin.call('my_plugin', 'namespace.function', arg1, arg2)
plugin.unload('my_plugin')
plugin.status()
```

Loading requires an **exact allowlist entry**. Trusted paths come from:

1. `debug.dejavu.plugins.allowed` + `.0` … `.15` — root-set absolute paths
   (readable from the app namespace and refreshed on every load); or
2. `/data/adb/modules/dejavu_zygisk/config/plugins.txt` when the app namespace
   can access the module directory.

An empty allowlist rejects every plugin; there is no implicit "module/tmp/app
data" prefix fallback. `plugin install`/`plugin allow` manage the property
slots. Plugin files must be regular (non-symlink), not group/world writable,
and owned by root or the target uid. Optional per-slot SHA-256 hashes bind the
path to expected file bytes.
On Android, the app process may also need executable file permissions and a
loadable SELinux label (for example mode `755` and
`chcon u:object_r:apk_data_file:s0` for an app-files plugin).

## Auto-loading early

Set a colon/space-separated list in the system property
`debug.dejavu.plugins` before the target process starts. The agent loads those
plugins in `HookManager::initialize()`, before app code runs, which is useful
for hooks that must observe process startup. Plugin Lua registrations are
queued until the control session exists, then registered automatically.

Unloading refuses active callbacks, revokes registered and returned Lua host
functions, retires stream handles, and calls optional `on_unload_prepare`
without holding the manager mutex. The host then removes remaining tracked
hooks, calls `shutdown`, and finally `dlclose()`s (or releases the cached JIT
module). Failed hook removal retains the module so unload can be retried.
A plugin is refused unless its descriptor sets
`DEJAVU_PLUGIN_FLAG_UNLOAD_SAFE`; in-flight functions are also refused.
There is currently no automatic re-load; load the path again.

Host functions returned as `DEJAVU_VALUE_HOST_FUNCTION` are wrapped in the
same revocable dispatch as registered functions, including functions returned
by another returned function. They restore the plugin's identity, count as
in-flight during execution, and fail safely after unload or reload. Repeated
returns of the same function/context reuse their binding. Returned callback
contexts remain plugin-owned and must stay valid while callable; the wrapper
does not free them. Bindings remain allocated until the manager is destroyed
so Lua closures cannot retain freed metadata. Borrowed string results are
copied before the plugin's in-flight reference is released; handles remain
opaque caller-managed values.

**Important:** hooks installed outside the host interface (e.g. direct Dobby
calls) are not tracked and must be removed by the plugin before shutdown
completes, otherwise they keep pointing into the unloaded library.

## JIT (TinyCC) plugins

`plugin.load_jit(device_path)` reads a trusted C source (allowlist + optional
SHA-256), prepends the prelude generated from
[`include/dejavu_jit.h`](../../include/dejavu_jit.h), compiles it in-process with
`dejavu_engine_compile()` and calls `dejavu_jit_plugin_init()`. JIT plugins are
C-only, compiled with `-nostdlib`, need the Lua control engine, and are loaded
on demand (not through early autoload). Source size limit: 512 KiB.

Optional exports:

```c
const char *dejavu_jit_plugin_name(void);
const char *dejavu_jit_plugin_version(void);
int dejavu_jit_plugin_unload_safe(void);
int dejavu_jit_plugin_capabilities(void);
void dejavu_jit_plugin_control_ready(void);
void dejavu_jit_plugin_unload_prepare(void);
int dejavu_jit_plugin_init(void);
void dejavu_jit_plugin_shutdown(void);
```

The prelude exposes `dejavu_jit_register`, alloc/free/log/classloader helpers,
`dejavu_jit_mem_*`, `dejavu_jit_module_resolve`, `dejavu_jit_hook_*`,
`dejavu_jit_fs_*`, `dejavu_jit_stream_*` (TCP/UDP/KCP) and the typed
arg/return helpers. Compiled modules are
cached by source SHA-256 (up to four entries); unload releases the reference and
reload of the same source hits the cache (`JIT cache hit`), so JIT static/global
state can survive reload until PluginManager shutdown. Example:
[`plugins/hello/hello_jit.c`](../../plugins/hello/hello_jit.c).

## Example

[`plugins/README.md`](../../plugins/README.md) indexes the examples.
[`plugins/hello/hello_hook_jit.c`](../../plugins/hello/hello_hook_jit.c)
demonstrates host-owned hook teardown: it installs an `atoi` hook and calls
`dejavu_jit_hook_uninstall` from `dejavu_jit_plugin_unload_prepare`. The host
also retries any remaining tracked hook removal before releasing its module.

[`plugins/hello/`](../../plugins/hello/plugin_entry.cpp) is a small contract
plugin that exercises host services (mem query, module resolve), plugin
registration and safe unload. [`plugins/unpack/`](../../plugins/unpack/README.md)
is a larger dynamically loaded plugin covering DEX capture and active
invocation; it is not unload-safe and must not be unloaded while its hooks are
installed. Both are examples, not part of the framework ABI.
