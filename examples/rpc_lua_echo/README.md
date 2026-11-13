# Lua RPC Echo

This source-only plugin demonstrates `rpc.register`, ordinary Lua table parameters
and results, business errors, cancellation and retryable service cleanup. It
implements Manager's existing `start`/`stop`/`status` package lifecycle. Requests
and responses stay in memory; the example stores only an in-memory call count.

Build a Manager-importable ZIP from the project root (no NDK required):

```sh
mkdir -p out/plugins
(cd examples/rpc_lua_echo && zip -q ../../out/plugins/rpc_lua_echo.zip manifest.json main.lua)
```

Install the module containing Lua RPC support. In Manager, start and configure
**Configuration → Router**, import the ZIP with **+**, and select **Settings** in
this plugin's Scope. Start the target process and the Lua plugin. The registered
route `example.lua.echo` appears in Router. Use only one process for this fixed
example route; real account plugins should publish distinct routes per account
and change them with `service:update_routes({...})`.

The Hub sends one JSON line:

```json
{"type":"request","id":"lua-echo-1","route":"example.lua.echo","method":"echo","params":{"message":"hello","items":[1,true,null]},"deadline_ms":2000}
```

The result equals `params`. `status` reports the service name and call count;
`fail` returns `example.rejected`. Stop the plugin to withdraw its route. A busy
stop retains the service and can be retried without losing its Lua callbacks.

Read the [Lua RPC contract](../../zygisk/docs/plugin-rpc.md#lua-bindings) before
adding application APIs. This example performs no account lookup or external
business operation. It needs no native plugin and does not open a socket itself.

The example uses the default Hub. To assign it a dedicated Hub, copy
`router.example.json` to `router.json`, edit its host/port, add
`"router": "router.json"` to the manifest and include `router.json` in `files`
and the ZIP. Import the package, then reload plugin configurations in Manager's
Router page. Its upstream appears as `rpc_lua_echo/main`; the Lua code and local
Unix endpoint stay the same. Use Manager for device-local credentials and
endpoint overrides. See the [routing configuration contract](../../router/plugin-config.md)
for multiple upstreams and route assignments.
