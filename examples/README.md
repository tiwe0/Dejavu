# Example plugins

These **examples and ABI contract tests** are packaged separately from the
framework. The agent loads native shared objects through `plugin.load()` or
native autoload, compiles JIT sources through `plugin.load_jit()`, and runs
Lua packages from Manager.

## Router integration matrix

Five of the example plugins integrate directly with **Dejavu Router** (v1 and
v2 + TLS) through the framework-owned local Unix RPC service (`abstract:dejavu.router.plugins`).
By default they route through the active default Hub configured in Manager;
each also ships a `router.example.json` (Router v2 + TLS template) for assigning
a dedicated upstream.

| Plugin | Kind | Router service | Route | RPC methods | Unload-safe |
| --- | --- | --- | --- | --- | --- |
| [`capture/`](capture/) | Native | `capture` | `capture.<process>` | `status`, `start`, `stop`, `recent`, `clear`, `probe` | Yes |
| [`algorithm/`](algorithm/) | Native + Lua | `algorithm` | `algorithm.<process>` | `status`, `start`, `stop`, `recent`, `clear`, `selftest` | Yes |
| [`unpack/`](unpack/README.md) | Native + Lua | `unpack` | `unpack.<process>` | `status`, `enable`, `disable`, `hook_invoke`, `active_invoke_start`, `active_invoke_status`, `dump_revision`, `flush` | No |
| [`rpc_echo/`](rpc_echo/README.md) | Native | `rpc_echo` | `example.echo` | `echo`, `status`, `ping`, `fail` | Yes |
| [`rpc_lua_echo/`](rpc_lua_echo/README.md) | Lua (+ `ui.json`) | `rpc_lua_echo` | `example.lua.echo` | `echo`, `ping`, `status`, `reset_stats`, `fail` | Yes (`stop`) |

Additional ABI / JIT examples:

- [`hello/`](hello/plugin_entry.cpp) — minimal unload-safe plugin exercising
  `mem.query`, `module.resolve`, registration and safe unload.
- [`hello/hello_jit.c`](hello/hello_jit.c) — TinyCC JIT host ABI example (`hellojit`).
- [`hello/hello_fs_jit.c`](hello/hello_fs_jit.c) — TinyCC JIT filesystem example (`hellofsjit`).
- [`hello/hello_hook_jit.c`](hello/hello_hook_jit.c) — TinyCC JIT tracked-hook lifecycle example (`hellohookjit`).
- [`hello/device-test.sh`](hello/device-test.sh) — automated on-device native/JIT smoke test.

## Build, test and package

Host contract tests (no Android NDK required):

```sh
./examples/rpc_echo/test-host.sh
./examples/capture/test-host.sh
./examples/algorithm/test-host.sh
```

Build all five native examples and package the release bundle + single-plugin
Manager ZIPs:

```sh
./examples/hello/build.sh
./examples/capture/build.sh
./examples/algorithm/build.sh
./examples/unpack/build.sh
./examples/rpc_echo/build.sh
./examples/package.sh
```

Outputs under `out/plugins/`:

- `dejavu-plugins-vX.Y.Z-arm64.zip` — combined release bundle containing all
  nine `<plugin>/manifest.json` folders, `router.example.json` templates,
  `unpack/cli.py`, and ready-to-import `zips/<plugin>.zip` archives.
- `out/plugins/zips/<plugin>.zip` — individual single-plugin ZIPs ready for
  Manager's **Import ZIP** action (no manual extraction or re-zipping needed).

## Installing in Manager or via CLI

- **Manager (recommended)**: Extract `zips/<plugin>.zip` from the release
  bundle (or use `out/plugins/zips/<plugin>.zip` directly), open **Plugins →
  Import ZIP** in Manager, select the target app in **Scope**, and load/start
  the plugin. If Router is running, its route appears automatically in
  **Configuration → Router**.
- **Direct library copy**: Copy extracted `<plugin>/` folders directly under
  `/data/adb/dejavu/plugins` and refresh the Plugins list in Manager.
- **CLI one-stop install**:

```sh
./dejavu plugin -p <pkg> install out/plugins/capture/libdejavu_capture.so \
    --skill examples/capture/SKILL.md
./dejavu plugin -p <pkg> install examples/hello/hello_jit.c
```

## Dedicated Router v2 + TLS upstream per plugin

Plugins without `"router": "router.json"` in `manifest.json` use the default
Hub. To bind a plugin to a separate Hub (including Router v2 binary framing
over TLS):

1. Copy `<plugin>/router.example.json` to `<plugin>/router.json` and edit
   `host`, `port`, `protocol` (`"v1"` or `"v2"`), `tls` and `tls_sni`.
2. Add `"router": "router.json"` to `manifest.json` and list `"router.json"`
   in `"files"`.
3. Import the ZIP in Manager, then tap **Configuration → Router → Reload plugin
   configurations** and set `device_token` via Manager's device-local override.

See [`router/plugin-config.md`](../router/plugin-config.md),
[`zygisk/docs/plugin-rpc.md`](../zygisk/docs/plugin-rpc.md), and
[`zygisk/docs/plugin-api.md`](../zygisk/docs/plugin-api.md) for the full
contracts.
