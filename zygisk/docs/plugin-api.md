# Native plugins

Dejavu can load native plugins at runtime instead of compiling every feature
into the agent. A plugin is an arm64 shared object exporting one C entry point:

```c
#include <dejavu_plugin.h>

extern "C" __attribute__((visibility("default")))
const DejavuPluginDescriptor *dejavu_plugin_entry(void);
```

The descriptor contains an ABI version, a name/version and `initialize` /
`shutdown` callbacks. `initialize` receives a `DejavuPluginHost` table with
stable function pointers:

- `register_function(host, namespace, name, function, context, error, size)`
  registers a Lua function (the plugin does not link against agent internals);
- `log_message` writes to logcat under `DejavuPlugin`;
- `get_app_class_loader` returns the current app ClassLoader (or `NULL` before
  Lua control is initialized).

The ABI lives in [`include/dejavu_plugin.h`](../../include/dejavu_plugin.h) and
is versioned with `DEJAVU_PLUGIN_ABI_VERSION`. Plugins are built with
`-fvisibility=hidden`; only `dejavu_plugin_entry` is exported.

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
`/data/data/`). On Android, the app process may also need executable file
permissions and a loadable SELinux label (for example mode `755` and
`chcon u:object_r:apk_data_file:s0` for an app-files plugin).

## Auto-loading early

Set a colon/space-separated list in the system property
`debug.dejavu.plugins` before the target process starts. The agent loads those
plugins in `HookManager::initialize()`, before app code runs, which is useful
for hooks that must observe process startup. Plugin Lua registrations are
queued until the control session exists, then registered automatically.

Unloading calls the plugin's `shutdown`, marks its registered Lua functions
invalid (they return "plugin function is no longer loaded") and `dlclose()`s
the library. There is currently no automatic re-load; load the path again.

**Important:** a plugin that installed native inline hooks must remove them in
`shutdown()` before returning; otherwise hooks keep pointing into the unloaded
library and the next call crashes. Plugins should only be unloaded when their
hook state can be fully torn down.

## Example

[`plugins/unpack/`](../../plugins/unpack/README.md) is a dynamically loaded
plugin that demonstrates DEX capture and active invocation. It is an example,
not part of the framework ABI.
