# Lua unpack strategy example

This directory is the reference **strategy layer**, written entirely in Lua:

1. `manifest.lua` resolves the launcher activity class through Java reflection and
   derives its package (the namespace).
2. `dex.lua` parses dumped DEX/CompactDex files and lists class names in that
   namespace (or matching a substring filter).
3. `main.lua` drives the collection: it hooks `ArtMethod::Invoke`, calls one
   class at a time in a paced loop (`sleep_ms` between batches), then asks for a
   revision dump and flushes the deferred writer.

Only hook/dump primitives are native (`unpack.enable`, `unpack.hook_invoke`,
`unpack.active_invoke`, `unpack.dump_revision`, `unpack.flush`,
`unpack.sleep_ms`), so the strategy itself is readable and editable in Lua.

## Usage

```sh
# capture DEX while the target runs (native hooks + deferred writer)
python3 examples/unpack/cli.py -p <process> --out <device-dir>

# Lua strategy: parse the manifest main-activity namespace from dumped dex,
# then invoke classes slowly (40 ms between batches), then flush
python3 examples/unpack/cli.py -p <process> --out <device-dir> \
    --active-invoke-manifest [--class-filter substring]
```

For a specific dumped dex:

```sh
python3 examples/unpack/cli.py -p <process> --out <device-dir> \
    --active-invoke-dex /data/.../register_dexfile_000.dex
```

## Plugin build

The native support sources live in `src/` and are built as a **dynamically
loaded plugin** (not compiled into the agent). It uses the host hook interface
(`host->get_interface("hook")`) instead of linking Dobby directly, and is not
marked unload-safe because its installed native hooks are not removed:

```sh
./examples/unpack/build.sh
# -> out/plugins/unpack/libdejavu_unpack.so
```

`cli.py` builds the plugin if needed, pushes it to the target app's files
directory, applies `chmod 755` / a loadable SELinux label, sets
`debug.dejavu.plugins` + `debug.dejavu.unpack.dir`, and restarts the target.
The agent loads it through the generic `plugin.load`/`debug.dejavu.plugins`
mechanism; the ABI is `include/dejavu_plugin.h` and framework docs are
`zygisk/docs/plugin-api.md`.

Advanced: concatenate `dex.lua`, `manifest.lua`, `main.lua` and call
`run_unpack_example({dex_dir=..., class_filter=..., delay_ms=40,
fallback_classes='java.lang.Object'})` through `./dejavu exec`.

The example intentionally stays small: class enumeration currently comes from
dumped DEX files; hooking ART DexFile's internal class list is a future native
primitive.
