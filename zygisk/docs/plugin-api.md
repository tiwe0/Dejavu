# Native plugins

Dejavu can load native plugins at runtime instead of compiling every feature
into the agent. A plugin is an arm64 shared object exporting one C entry point:

```c
#include <dejavu_plugin.h>

extern "C" __attribute__((visibility("default")))
const DejavuPluginDescriptor *dejavu_plugin_entry(void);
```

The descriptor contains `abi_version`, `struct_size`, name/version, flags,
`initialize`, `on_control_ready` and `shutdown`. All ABI structs are
append-only and must be validated with `struct_size`; v1 is frozen.

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

`on_control_ready` is called once the Lua control session exists; plugins that
need the control session/ClassLoader should defer that work there instead of
doing it in `initialize`.

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
  interface are **owned by the calling plugin**: the host records them and
  destroys any remaining hooks automatically on unload, before destroying the
  plugin module. Plugins that install hooks outside this interface remain
  responsible for their own teardown.

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

Loading is restricted to absolute paths under the module directory,
`/data/local/tmp/`, or app data directories (`/data/user/`, `/data/user_de/`,
`/data/data/`). Trusted paths come from:

1. `debug.dejavu.plugins.allowed` — colon/space-separated absolute paths
   (root-set and readable from the app namespace), refreshed on every load; or
2. `/data/adb/modules/dejavu_zygisk/config/plugins.txt` when the app namespace
   can access the module directory.

If either source is non-empty, it is an exact allowlist: only listed plugins
may load. Set the property when the manifest is not reachable from the app
process. Plugin files must be regular (non-symlink), not group/world writable,
and owned by root or the target uid.
On Android, the app process may also need executable file permissions and a
loadable SELinux label (for example mode `755` and
`chcon u:object_r:apk_data_file:s0` for an app-files plugin).

## Auto-loading early

Set a colon/space-separated list in the system property
`debug.dejavu.plugins` before the target process starts. The agent loads those
plugins in `HookManager::initialize()`, before app code runs, which is useful
for hooks that must observe process startup. Plugin Lua registrations are
queued until the control session exists, then registered automatically.

Unloading calls the plugin's `shutdown`, marks its registered Lua functions
invalid (they return "plugin function is no longer loaded") and `dlclose()`s
the library. A plugin is refused unless its descriptor sets
`DEJAVU_PLUGIN_FLAG_UNLOAD_SAFE`; in-flight functions are also refused.
There is currently no automatic re-load; load the path again.

**Important:** a plugin that installed native inline hooks must remove them in
`shutdown()` before returning; otherwise hooks keep pointing into the unloaded
library and the next call crashes. Plugins should only be unloaded when their
hook state can be fully torn down.

## Example

[`plugins/README.md`](../../plugins/README.md) indexes the examples.
[`plugins/hello/hello_hook_jit.c`](../../plugins/hello/hello_hook_jit.c)
demonstrates host-owned hook teardown: it installs an `atoi` hook and relies on
`plugin.unload` to remove it.

[`plugins/hello/`](../../plugins/hello/plugin_entry.cpp) is a small contract
plugin that exercises host services (mem query, module resolve), plugin
registration and safe unload. [`plugins/unpack/`](../../plugins/unpack/README.md)
is a larger dynamically loaded plugin covering DEX capture and active
invocation; it is not unload-safe and must not be unloaded while its hooks are
installed. Both are examples, not part of the framework ABI.
