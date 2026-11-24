# RPC Echo

A native plugin example for the [business RPC contract](../../zygisk/docs/plugin-rpc.md).
The plugin has no socket code, reconnect loop or wire-envelope parsing. It declares
route `example.echo` and four callbacks:

| Method | Result |
| --- | --- |
| `echo` | Returns the validated params JSON unchanged and increments `requests` |
| `ping` | Returns `{"pong":true,"service":"rpc_echo","route":"example.echo","requests":N}` |
| `status` | Returns `service`, `version`, `route`, `requests`, `errors`, and `pid` |
| `fail` | Increments `errors` and returns the documented `example.rejected` business error |

The framework connects to `abstract:dejavu.router.plugins` by default, verifies the
root daemon, dispatches callbacks and manages cancellation and unload safety.
The example stores no requests or responses.

```sh
source out/tooling/env.sh
examples/rpc_echo/test-host.sh
examples/rpc_echo/build.sh
examples/package.sh
```

`examples/package.sh` writes a Manager-importable archive directly to
`out/plugins/zips/rpc_echo.zip`. Import it in Manager via **Plugins → Import ZIP**,
choose an authorized target's Scope, and load it into one process (loading the
same fixed route into multiple processes intentionally triggers a route conflict).

Once Router is running (v1 or v2 + TLS) and the plugin is loaded, its route is
announced. A Hub request can use:

```json
{"type":"request","id":"echo-test","route":"example.echo","method":"echo","params":{"value":"hello"},"deadline_ms":2000}
{"type":"request","id":"ping-test","route":"example.echo","method":"ping","params":{},"deadline_ms":2000}
```

To bind `rpc_echo` to a dedicated Router v2 + TLS Hub, copy
[`router.example.json`](router.example.json) to `router.json`, declare
`"router": "router.json"` in `manifest.json`, and reload plugin configurations
in Manager's Router page.
