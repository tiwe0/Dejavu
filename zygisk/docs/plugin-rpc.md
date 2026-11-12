# Plugin business RPC contract (v1)

The Agent's `rpc` host service connects native business plugins to the built-in
router. Plugins declare their methods and routes; the framework owns connection,
registration, framing, reconnection, deadlines and response delivery. The router
runs at `/data/adb/dejavu/bin/dejavuerouter` independently of Manager.

The public definitions are in [`include/dejavu_plugin.h`](../../include/dejavu_plugin.h).
A complete, buildable implementation is [`examples/rpc_echo`](../../examples/rpc_echo/README.md).

## Responsibility and versioning

| Framework owns | Business plugin owns |
| --- | --- |
| Unix socket and peer identity checks | Discovering the current business route/account |
| Connection, hello handshake and reconnect | Declaring supported method names |
| Request IDs, wire envelopes and method dispatch | Validating method-specific parameters and permissions |
| Bounded admission, deadline and cancellation state | Calling application APIs/JNI and producing result/error JSON |
| Allocator ownership and unload quiescence | Checking cancellation before expensive or irreversible work |

Business plugins do not open a router socket, emit protocol envelopes, run a
transport reconnect loop or inspect Manager internals. A business-specific
account observer may call `update_routes` when its current route changes.

The plugin ABI remains **v2**. The separate host interface version is
`DEJAVU_RPC_INTERFACE_VERSION == 1`; plugins declare `DEJAVU_PLUGIN_CAP_RPC`
(`0x40`) and request `host->get_interface(host->host_context, "rpc", 1)`.
Unsupported interfaces or missing capability return `NULL`. Check `version`,
`struct_size` and every function pointer used before registration. Existing
host/descriptor layout and unrelated interfaces remain compatible.

The RPC interface version describes transport and callback ABI, not a business
schema. Add a new method name, such as `lookup.v2`, when changing the meaning or
required fields of an existing method. Keep the original method until its
callers have migrated. A plugin's descriptor version identifies its own release.

## Service and method declaration

```c
static const DejavuRpcMethod methods[] = {
    {"echo", on_echo},
    {"status", on_status},
};
static const char *routes[] = {"example.echo"};
DejavuRpcServiceDescriptor service = {
    sizeof(DejavuRpcServiceDescriptor),
    "rpc_echo", "example.rpc_echo", routes, 1,
    methods, 2, state,
    NULL, /* default Unix endpoint */
};
uint64_t handle = 0;
int rc = rpc->register_service(host->host_context, &service, &handle,
                               error, sizeof(error));
```

The descriptor supplies a stable service name, application identity, unique route
strings, immutable method table and callback context. The framework copies the
descriptor's strings, routes and method table during registration. The callback
function addresses and `context` must remain valid until unregistration or host
retirement has drained the service.

A handle belongs to the stable plugin-instance owner, not its position in the
loader's plugin list. A different plugin cannot update or unregister it. Keep the
host table and `host_context` received by this instance; do not share handles or
contexts between instances.

Registration may happen in `initialize`, but callbacks are not admitted until
initialization and control readiness succeed. Registration with zero routes
creates an offline service. Publish routes when the account or other business
identity becomes available. A plugin that requires its ClassLoader can register
from `on_control_ready` instead. A successful `initialize` must assign
`*plugin_state`; an initializer that fails must release its own allocations and
workers before returning.

`update_routes(context, handle, routes, count, ...)` replaces the route set.
Zero routes withdraws the service. The v1 implementation reconnects and performs
a new hello handshake; it cancels the previous connection's requests and does
not replay them. Route conflicts or a failed handshake do not make a service
online merely because its socket connected.

## Request callback

```c
int on_echo(void *state, const DejavuRpcRequest *request,
            DejavuRpcResponse *response, char *error, size_t error_size);
```

Callbacks for one service run sequentially on a framework-owned dispatcher. I/O
continues on a separate worker, so disconnects and cancellation can be observed
while a handler runs. Different services may execute concurrently. Do not assume
the callback runs on the app main thread or inside a Lua control call.

`request` and its `id`, `route`, `method`, `params_json` and cancellation context
are borrowed for this callback only. `params_size` is the JSON byte count. Parse
and validate your method's fields and use the route to check the current business
identity before accessing account-specific data. Copy only the data your
synchronous operation needs; v1 does not support retaining the request and
completing it asynchronously after the callback returns.

`id` is an opaque transport correlation value. The router assigns a fresh value
to every dispatch and restores the caller's original ID when replying to the
Hub. Do not interpret it as a business identifier or use it for deduplication.

`deadline_monotonic_ms` is an absolute `CLOCK_MONOTONIC` deadline on the device,
not a wall-clock timestamp or a duration. Check
`request->is_cancelled(request->cancellation_context)` before expensive work and
at natural interruption points. Cancellation covers timeout, disconnect, route
replacement and service retirement. It does not undo a business operation that
has already happened. A blocking application or JNI call must return before
its native callback can finish.

JNI environment pointers are thread-local. A handler needing JNI must obtain its
own `JNIEnv` from `host->java_vm`, attach its current thread if necessary, and
detach only if it attached that thread. The ClassLoader returned by
`host->get_app_class_loader` is a borrowed host-owned global reference. Resolve
application classes through that loader; `FindClass` on a native worker is only
suitable for bootstrap classes.

## Response and allocation ownership

A completed business operation returns `DEJAVU_RPC_OK` and fills:

```c
response->struct_size = sizeof(*response);
response->ok = 1;
response->payload_json = host_allocated_json;
response->payload_size = json_byte_count;
```

`payload_json` must be allocated with this host's `alloc` and contain complete
JSON. `payload_size` excludes a trailing NUL. The framework consumes and frees
this allocation after the callback, including rejected, cancelled and failing
outcomes. Never return stack memory, a `std::string::c_str()`, a cJSON allocation
or a plugin allocator's block. Serialize cJSON into a temporary string, copy it
into `host->alloc`, then release the temporary with its own allocator.

- `ok == 1`: the JSON becomes the wire response's `result`.
- `ok == 0`: the JSON is a business error object with string `code` and `message`,
  and an optional boolean `retryable`. Return `DEJAVU_RPC_OK` for this completed
  business rejection as well.
- Return `DEJAVU_RPC_ERROR` for a callback/host failure, with a concise diagnostic
  in `error`. It is not how to return a business error payload.

For example:

```json
{"code":"example.rejected","message":"Example business rejection","retryable":false}
```

The `rpc.*` error-code prefix is reserved for framework failures. Business
plugins keep their own documented domain codes; unknown methods are rejected by
the framework before entering a callback.

| Framework code | Meaning |
| --- | --- |
| `rpc.route_mismatch` | Request route is not owned by this service generation |
| `rpc.method_not_found` | No declared callback matches the method |
| `rpc.duplicate_id` | A request ID is already pending on this connection |
| `rpc.busy` | The service's bounded pending queue is full |
| `rpc.invalid_response` | Callback result violates the JSON/ownership contract |
| `rpc.handler_failed` | Callback failed or threw rather than completing a result |

Cancellation and deadlines discard late callback results; the daemon already
owns the corresponding request timeout/cancel response. Do not include credentials,
complete requests, tokens or response bodies in diagnostics or logcat.

## Wire transport and operational limits

The default endpoint is **`abstract:dejavu.router.plugins`** on Android. It is a
named abstract Unix socket and has no filesystem node. The client checks that
the server runs as root before the handshake. There is no implicit TCP fallback.
For host tests, an explicitly configured `unix:/absolute/path` endpoint uses a
filesystem Unix socket. The plugin normally leaves `endpoint` as `NULL`.

The router and framework exchange one JSON value per line:

```json
{"type":"hello","app":"example.rpc_echo","plugin":"rpc_echo","version":1,"routes":["example.echo"]}
{"type":"hello_ok","version":1}
{"type":"request","id":"job-1","route":"example.echo","method":"echo","params":{"value":"hello"},"deadline_monotonic_ms":123456789}
{"type":"response","id":"job-1","route":"example.echo","ok":true,"result":{"value":"hello"}}
{"type":"cancel","id":"job-1","route":"example.echo"}
```

Business callbacks neither build nor parse those envelopes. The framework waits
for `hello_ok`, bounds handshake time, rejects oversized/malformed input and
limits the service's pending work. A process supports at most 16 services. Each
service supports 0–64 unique routes, at most 128 bytes per route, and 64 method
declarations. Service/method names use ASCII letters, digits, `_`, `.` or `-`,
up to 128 bytes; the application identity is at most 256 non-control bytes. Wire
lines are limited to 1 MiB, with at most 16 queued requests and one active
callback per service. Applications should keep individual business results comfortably
below the wire limit because the response envelope consumes additional bytes.
The router also checks the final Hub response after restoring its caller ID;
an oversized result or error becomes `response_too_large` for that request.

Disconnected, timed-out or cancelled requests are never automatically replayed.
A result produced for an old connection cannot be delivered over a new one.
`retryable` is information for the caller; it does not instruct the framework to
retry. Callers must treat lost responses to mutating work as an unknown outcome.

## Stop and unload

`unregister_service` stops admission, withdraws the connection, cancels queued and
active requests and waits for callbacks to drain. `DEJAVU_RPC_BUSY` means that a
callback is still running: keep its code, context and state alive and retry.
It does not authorize destroying that state or calling `dlclose`.

The loader retires an owner's RPC services before plugin unload callbacks and
checks quiescence before shutdown/dlclose. A plugin whose callback cannot yet
return stays mapped. All callback waits and worker joins happen outside the
PluginManager mutex. Business plugins still stop and join any account observers
or other workers they created in their unload hooks.

Only configuration is persistent. RPC admission queues, requests, responses,
connection state and monitor history stay in memory. Production secrets belong
to the business handler's existing storage contract, not to the RPC framework.

## Verification

```sh
examples/rpc_echo/test-host.sh
examples/rpc_echo/build.sh
zygisk/tests/test-rpc-service.sh
```

The example checks the callback contract without an Android account. Framework
contract tests exercise Unix handshake, reconnection, route replacement,
deadlines, cancellation, ownership and unload quiescence. Device acceptance should
first use `echo` or `status`, then separately authorize any real business operation.
