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

`initialize` receives a `DejavuPluginHost` table with stable function pointers:

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
(`DEJAVU_PLUGIN_CAP_MEM/MODULE/HOOK/FS/JAVA`); `plugin.list()` prints them as
`caps=0x...`. Read the field only when `struct_size` reaches its offset.
Capabilities are enforced: `get_interface("mem"|"module"|"hook"|"fs")` and
the corresponding `dejavu_jit_*` helpers return failure unless the plugin
declared the matching bit. A plugin that resolves `libc.so` symbols and then
installs a hook needs `HOOK|MODULE`; declare every service actually used.

`on_control_ready` is called once the Lua control session exists; plugins that
need the control session/ClassLoader should defer that work there instead of
doing it in `initialize`.

`on_unload_prepare` (read only when `struct_size` reaches its offset) is called
before the unload-hook check; plugins install native hooks here and remove them
before returning. If any host-tracked hook remains, unload is refused. The
plugin's Lua entries are invalidated before `shutdown`/module teardown.

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
- **hook** (`DEJAVU_HOOK_INTERFACE_VERSION`): `install(target, replacement,
  &original)` via Dobby and `uninstall(target)`. Hooks installed through this
  interface are owned by the calling plugin; duplicate targets and
  non-owner uninstalls are rejected. `plugin.unload` is refused while the
  plugin still owns any host-tracked hook: the plugin must call
  `uninstall(target)` (usually from its shutdown path) before unload. Remaining
  hooks are torn down only at process/PluginManager shutdown or on a failed
  initialization.

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

Unloading first calls optional `on_unload_prepare`, verifies that no
host-tracked hook remains, invalidates registered Lua functions, calls
`shutdown`, and finally `dlclose()`s (or releases the cached JIT module).
A plugin is refused unless its descriptor sets
`DEJAVU_PLUGIN_FLAG_UNLOAD_SAFE`; in-flight functions are also refused.
There is currently no automatic re-load; load the path again.

**Important:** host-tracked hooks must be uninstalled by the plugin before
unload; the host enforces this. Hooks installed outside the host interface
(e.g. direct Dobby calls) are not tracked and the plugin must remove them in
`shutdown()`, otherwise they keep pointing into the unloaded library.

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
`dejavu_jit_fs_*`, and the typed arg/return helpers. Compiled modules are
cached by source SHA-256 (up to four entries); unload releases the reference and
reload of the same source hits the cache (`JIT cache hit`), so JIT static/global
state can survive reload until PluginManager shutdown. Example:
[`plugins/hello/hello_jit.c`](../../plugins/hello/hello_jit.c).

## Example

[`plugins/README.md`](../../plugins/README.md) indexes the examples.
[`plugins/hello/hello_hook_jit.c`](../../plugins/hello/hello_hook_jit.c)
demonstrates host-owned hook teardown: it installs an `atoi` hook and calls
`dejavu_jit_hook_uninstall` from `dejavu_jit_plugin_unload_prepare`; the host
refuses unload while the hook is still tracked.

[`plugins/hello/`](../../plugins/hello/plugin_entry.cpp) is a small contract
plugin that exercises host services (mem query, module resolve), plugin
registration and safe unload. [`plugins/unpack/`](../../plugins/unpack/README.md)
is a larger dynamically loaded plugin covering DEX capture and active
invocation; it is not unload-safe and must not be unloaded while its hooks are
installed. Both are examples, not part of the framework ABI.
