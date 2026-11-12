# RPC Echo

A native plugin example for the [business RPC contract](../../zygisk/docs/plugin-rpc.md).
The plugin has no socket code, reconnect loop or wire-envelope parsing. It declares
route `example.echo` and three callbacks:

| Method | Result |
| --- | --- |
| `echo` | Returns the validated params JSON unchanged |
| `status` | Returns the example service and version |
| `fail` | Returns the documented `example.rejected` business error |

The framework connects to `abstract:dejavu.router.plugins` by default, verifies the
root daemon, dispatches callbacks and manages cancellation and unload safety.
The example stores no requests or responses.

```sh
source out/tooling/env.sh
examples/rpc_echo/test-host.sh
examples/rpc_echo/build.sh
```

Build output is `out/plugins/rpc_echo/`. ZIP only the three declared runtime
resources to import it in Manager:

```sh
(cd out/plugins/rpc_echo && zip -q ../rpc_echo.zip manifest.json libdejavu_rpc_echo.so SKILL.md)
```

Alternatively, install and trust the library through the existing native plugin workflow.
Choose an authorized target's Scope and load the plugin into one process; loading
the same fixed route into multiple processes intentionally causes a route conflict.

Once the internal router is running and the plugin's control session is ready,
its route is announced. A Hub request can use:

```json
{"type":"request","id":"echo-test","route":"example.echo","method":"echo","params":{"value":"hello"},"deadline_ms":2000}
```

The result is `{"value":"hello"}`. No application account or credentials are needed.
