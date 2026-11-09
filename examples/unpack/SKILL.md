---
name: dejavu-plugin-unpack
description: Example Dejavu plugin demonstrating hook-based DEX capture and FART-style active invocation. Use only on owned devices or with explicit authorization.
---

# unpack plugin

This is the **larger example plugin** from `examples/unpack`. It is loaded
dynamically through `plugin.load` and is **not** part of the framework ABI.
It captures DEX images at ART registration points and supports paced active
invocation for authorized extracting-shell research.

## Before you start

- Self-use / authorized testing only. Never run it against third-party apps or
  devices without permission.
- Verify the target's Application/context and that the agent is reachable.
- The plugin installs native ART hooks; it is **not unload-safe**.

## Build and deploy

```sh
./examples/unpack/build.sh          # -> out/plugins/unpack/libdejavu_unpack.so
python3 examples/unpack/cli.py -p <pkg> --out <device-dir>
```

`cli.py` builds/pushes the plugin, copies the Lua strategy, sets
`debug.dejavu.plugins.allowed`, loads it on demand after the app starts, and
calls `unpack.enable(<device-dir>)`.

## Lua strategy

The running plugin registers `unpack.*`:

```lua
unpack.enable('/data/user_de/0/<pkg>/files/dejavu-unpack')
unpack.hook_invoke()
unpack.active_invoke_async('com.example.A,com.example.B', 40)
unpack.active_invoke_wait(300000)
unpack.dump_revision()
unpack.flush(120000)
unpack.status()
```

The strategy layer (manifest namespace → dumped DEX class parser → paced active
invocation) lives in `examples/unpack/{manifest,dex,main}.lua` and is loaded as
part of the example Lua script; it is not native code.

## Safety / troubleshooting

- Prefer on-demand load after app startup; early ART hook installation during
  app init can ANR. `debug.dejavu.plugins` early autoload is for dedicated runs.
- DEX snapshots are queued in memory and flushed by a background writer; call
  `unpack.flush()` before pulling files.
- If the target restarts or the plugin is loaded twice, check
  `plugin.list()` / `plugins=... entries=...`.
- You can read this skill from the device with
  `./dejavu plugin skill unpack` when `SKILL.md` is copied next to the plugin.

## Key implementation files

- `examples/unpack/src/plugin_entry.cpp` — descriptor + registration
- `examples/unpack/src/unpack_manager.cpp` — DEX/ART hooks, sentinel, queue/writer
- `examples/unpack/design.md` — implementation details
