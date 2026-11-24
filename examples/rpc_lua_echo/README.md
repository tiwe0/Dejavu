# Lua RPC Echo

This source-only plugin demonstrates `rpc.register`, ordinary Lua table parameters
and results, business errors, cancellation, Manager inline controls (`ui.json`),
and retryable service cleanup. It implements Manager's `start`/`stop`/`status`
lifecycle plus `actions` (`self_check`, `reset_stats`).

Build a Manager-importable ZIP (no NDK required):

```sh
mkdir -p out/plugins/zips
(cd examples/rpc_lua_echo && zip -q ../../out/plugins/zips/rpc_lua_echo.zip manifest.json main.lua ui.json)
```

Or run `./examples/package.sh` after native builds to generate all single-plugin
ZIPs under `out/plugins/zips/`.

In Manager:

1. Start and configure **Configuration → Router** (Router v1 or v2 + TLS).
2. Open **Plugins → Import ZIP** and pick `rpc_lua_echo.zip`.
3. Select an authorized target in **Scope**, open the plugin's inline control
   page, and tap **Start**. The route `example.lua.echo` appears in Router.

Supported RPC methods:

| Method | Result |
| --- | --- |
| `echo` | Returns `params` unchanged and increments `requests` |
| `ping` | Returns `{pong = true, service = "rpc_lua_echo", route = ..., requests = N}` |
| `status` | Returns `{service, version, requests, errors}` |
| `reset_stats` | Resets in-memory `requests` and `errors` counters |
| `fail` | Increments `errors` and returns `example.rejected` |

Example Hub request:

```json
{"type":"request","id":"lua-echo-1","route":"example.lua.echo","method":"echo","params":{"message":"hello","items":[1,true,null]},"deadline_ms":2000}
```

The example uses the default Hub out of the box. To assign it a dedicated
Router v2 + TLS Hub, copy [`router.example.json`](router.example.json) to
`router.json`, edit `host`/`port`/`protocol`/`tls`/`tls_sni`, add
`"router": "router.json"` to `manifest.json` and `"files"`, import the ZIP,
and tap **Reload plugin configurations** in Manager's Router page. Configure
`device_token` via Manager's device-local override. See
[`router/plugin-config.md`](../../router/plugin-config.md).
