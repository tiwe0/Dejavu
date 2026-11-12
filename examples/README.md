# Example plugins

These **examples and ABI contract tests** are packaged separately from the
framework. The agent loads native shared objects through `plugin.load()` or
native autoload and compiles JIT sources through `plugin.load_jit()`.

- [`rpc_echo/`](rpc_echo/README.md) — framework-owned Unix RPC example with
  echo/status handlers, business errors and cooperative cancellation. See the
  [plugin RPC contract](../zygisk/docs/plugin-rpc.md).
- [`capture/`](capture/) — example SSL plaintext capture plugin using the host
  module+hook interfaces.
- [`algorithm/`](algorithm/) — algorithm self-disclosure tracer: native libcrypto
  hooks plus a Java crypto Lua tracer.
- [`hello/device-test.sh`](hello/device-test.sh) — automated on-device
  native/JIT lifecycle smoke test (`make plugin-device-test`).
- [`hello/hello_fs_jit.c`](hello/hello_fs_jit.c) — JIT plugin exercising the
  host fs interface (read/write/exists/remove) without libc.
- [`hello/hello_hook_jit.c`](hello/hello_hook_jit.c) — JIT example of removing
  a host-tracked hook in `dejavu_jit_plugin_unload_prepare`.
- [`hello/hello_jit.c`](hello/hello_jit.c) — TinyCC JIT version: `plugin jit`
  reads/compiles the C source in-process; exercises host mem/module services.
- [`hello/`](hello/plugin_entry.cpp) — minimal unload-safe plugin that
  exercises host services (`mem.query`, `module.resolve`), registration and
  safe unload. Built by `hello/build.sh`.
- [`unpack/`](unpack/README.md) — larger DEX capture/active-invocation example.
  Not unload-safe because it installs native ART hooks; it uses the host hook
  interface instead of linking Dobby directly. Built by `unpack/build.sh` and
  driven by `unpack/cli.py`.

One-stop install (push + permissions + allowlist + load):

```sh
./dejavu plugin -p <pkg> install out/plugins/hello/libdejavu_hello.so \
    --skill examples/hello/SKILL.md
./dejavu plugin -p <pkg> install examples/hello/hello_jit.c
```

Build all five native examples and package the release bundle.

```sh
./examples/hello/build.sh
./examples/capture/build.sh
./examples/algorithm/build.sh
./examples/unpack/build.sh
./examples/rpc_echo/build.sh
./examples/package.sh
```

`plugin install` uses `--skill` when provided. Otherwise it looks for
`<artifact>.SKILL.md`, then a sibling `SKILL.md`. It copies the selected file
next to the deployed artifact as `<device-artifact>.SKILL.md` and pins its
SHA-256. Loading rejects a missing or changed pinned sidecar. Read the loaded
plugin's sidecar through `plugin.skill(name)` or `./dejavu plugin skill <name>`.

Example plugins are built separately from the module. `examples/package.sh`
creates `dejavu-plugins-vX.Y.Z-arm64.zip` containing the native `.so` files,
their SKILL sidecars, JIT examples and `unpack/cli.py`.
Every plugin has its own folder and `manifest.json` declaring its identifier,
display name, version, description, kind, entry and resource files. The app only
recognizes manifest folders directly under `/data/adb/dejavu/plugins`; bare
`.so`/`.c` files and folders without manifests are ignored. Run
`python3 examples/prepare-library.py` after native builds to assemble the same
eight plugin directories under `out/plugins` without creating the release ZIP.

Manager's **+** action accepts a ZIP with exactly one `manifest.json`. Extract
the release bundle, then create a new ZIP containing one plugin folder and all
its declared resources. For a direct library scan, copy the extracted plugin
folders into `/data/adb/dejavu/plugins` and refresh the list. Use **Load** in
the plugin's details to deploy and hash-pin the selected native or JIT example
for the selected target. [Manager import guide](../app/README.md#zip-plugin-import)

CI uploads the zip as the
`dejavu-example-plugins` artifact and attaches the zip to the GitHub Release as
a second asset; the module zip remains framework-only.

The ABI and host services are documented in
[`zygisk/docs/plugin-api.md`](../zygisk/docs/plugin-api.md).
