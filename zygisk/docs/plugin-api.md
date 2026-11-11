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
optional `on_unload_prepare`. The loader requires ABI v2 and a `struct_size`
that covers `shutdown`. ABI structs are append-only. Read an appended field
only when `struct_size` covers its full extent.

`initialize` receives a per-instance `DejavuPluginHost` table with stable function
pointers and an opaque `host_context`. Save this instance's host table and pass
its context unchanged to host services, including from native worker or hook
callback threads. Contexts and stream handles are never shared between plugins.
An initializer that returns failure must stop and join any workers it started
before returning. The framework retires its streams and attempts to remove
tracked hooks. It skips `on_unload_prepare` and `shutdown` for an uninitialized
plugin.

- `host->register_function(host->host_context, namespace_name, function_name,
  function, function_context, error, error_size)` registers a Lua function
  during `initialize`. JIT plugins call `dejavu_jit_register` during
  `dejavu_jit_plugin_init`. Namespace and function names must be simple
  identifiers. The host rejects duplicate namespace/function pairs and reserves
  the `hook`, `fs`, `module`, `native`, `java`, `mem` and `plugin` namespaces.
- `host->log_message(host->host_context, priority, message)` writes to logcat
  under `DejavuPlugin`.
- `host->get_app_class_loader(host->host_context)` returns a host-owned global
  reference to the current app ClassLoader, or `NULL` before Lua control is
  initialized.
- `host->alloc(size)` and `host->free(pointer)` are agent-owned allocation
  helpers for plugin results.
- `host->get_interface(host->host_context, name, version)` looks up `mem`,
  `module`, `hook`, `fs` or `stream`. It returns `NULL` for unavailable
  interfaces, unsupported versions and undeclared capabilities.

The descriptor may append optional `capabilities` bits
(`DEJAVU_PLUGIN_CAP_MEM/MODULE/HOOK/FS/JAVA/STREAM`); `plugin.list()` prints them as
`caps=0x...`. A descriptor without the field defaults to zero capabilities.
Interface lookup and the corresponding `dejavu_jit_*` helpers require the
matching `MEM`, `MODULE`, `HOOK`, `FS` or `STREAM` bit. Declare every service
the plugin uses. JavaVM and ClassLoader access are direct host-table members.

`on_control_ready` runs once the Lua control session exists. Use it for work
that needs the session or ClassLoader.

The ABI lives in [`include/dejavu_plugin.h`](../../include/dejavu_plugin.h) and
is versioned with `DEJAVU_PLUGIN_ABI_VERSION`. Plugins are built with
`-fvisibility=hidden`; only `dejavu_plugin_entry` is exported.

## Host services

Each native service method takes the saved `host->host_context` as its first
argument. The signatures below omit that common parameter where abbreviated.

- **mem** (`DEJAVU_MEM_INTERFACE_VERSION`) provides same-process `read` and
  `write` via `process_vm_readv` and `process_vm_writev`. `query` returns
  mapping bounds and protection bits (1 read, 2 write, 4 execute).
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
  `host->get_interface(host->host_context, "stream", DEJAVU_STREAM_INTERFACE_VERSION_2)`
  (`DejavuStreamInterfaceV2`). `listen` is
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
path to expected file bytes. `plugin install` pins the artifact and any selected
sidecar. It checks `--skill`, then `<artifact>.SKILL.md`, then the sibling
`SKILL.md`, and installs the selected file as `<device-artifact>.SKILL.md`.
A pinned sidecar must be nonempty, fit the 128 KiB read limit and match its hash.
On Android, the app process may also need executable file permissions and a
loadable SELinux label (for example mode `755` and
`chcon u:object_r:apk_data_file:s0` for an app-files plugin).

## Auto-loading early

Set a colon/space-separated list of native plugin paths in
`debug.dejavu.plugins` before the target process starts. The agent loads them
during `HookManager::initialize()`, before installing the application
bootstrap hook. It queues Lua registrations until the control session exists.

## Unloading

Normal unload requires `DEJAVU_PLUGIN_FLAG_UNLOAD_SAFE`, or a nonzero return
from `dejavu_jit_plugin_unload_safe` for JIT. The host refuses unload while a
registered or returned Lua host function is in flight. Once it starts unload,
the host revokes those functions and retires the plugin's stream handles,
cancelling blocking stream reads and accepts before `on_unload_prepare`.

The optional `on_unload_prepare` must quiesce plugin-owned callbacks and stop
and join any workers that still use plugin code. The host then attempts to
remove remaining tracked hooks. A failed unpatch or protection restore keeps
the module mapped for a later `plugin.unload` retry. After cleanup succeeds,
the host calls `shutdown` when provided and closes the native library, destroys
an uncached JIT module or releases its cache reference. Both lifecycle callbacks
run without the manager mutex. Load the path again to reload the plugin.

If initialization fails and hook cleanup is pending, `plugin.unload(name)`
can retry cleanup even without the unload-safe flag. The host skips both
lifecycle callbacks for that failed initialization.

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
`dejavu_engine_compile()` and calls the required `int dejavu_jit_plugin_init(void)`
entry point. The initializer returns `0` on success and can set a failure message
with `dejavu_jit_set_error`. JIT plugins are C-only, compiled with `-nostdlib`,
and loaded on demand once the Lua control engine is available. The source size
limit is 512 KiB.

The following exports are optional.

```c
const char *dejavu_jit_plugin_name(void);
const char *dejavu_jit_plugin_version(void);
int dejavu_jit_plugin_unload_safe(void);
int dejavu_jit_plugin_capabilities(void);
void dejavu_jit_plugin_control_ready(void);
void dejavu_jit_plugin_unload_prepare(void);
void dejavu_jit_plugin_shutdown(void);
```

Without a name export, the loader uses the source filename without its
extension. The name must be a simple identifier. The version defaults to an
empty string, capabilities to zero and unload-safe to disabled.

The prelude exposes `dejavu_jit_register`, alloc/free/log/classloader helpers,
`dejavu_jit_mem_*`, `dejavu_jit_module_resolve`, `dejavu_jit_hook_*`,
`dejavu_jit_fs_*`, `dejavu_jit_stream_*` (TCP/UDP/KCP) and the typed
arg/return helpers. The per-process cache holds up to four modules keyed by
source SHA-256. If full, it evicts the least recently used unreferenced entry.
A compile with all four entries in use remains uncached. Unload releases a
cached module reference, so reloading the same source while the entry is still
cached may retain static or global state. Initialize that state for each load.
[`examples/hello/hello_jit.c`](../../examples/hello/hello_jit.c)

## Example

[`examples/README.md`](../../examples/README.md) indexes the examples.
[`examples/hello/hello_hook_jit.c`](../../examples/hello/hello_hook_jit.c)
demonstrates host-owned hook teardown: it installs an `atoi` hook and calls
`dejavu_jit_hook_uninstall` from `dejavu_jit_plugin_unload_prepare`. The host
also retries any remaining tracked hook removal before releasing its module.

[`examples/hello/`](../../examples/hello/plugin_entry.cpp) is a small contract
plugin that exercises host services (mem query, module resolve), plugin
registration and safe unload. [`examples/unpack/`](../../examples/unpack/README.md)
is a larger dynamically loaded plugin covering DEX capture and active
invocation; it is not unload-safe and must not be unloaded while its hooks are
installed. Both are examples, not part of the framework ABI.
