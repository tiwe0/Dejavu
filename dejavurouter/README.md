# dejavurouter

A small, single-threaded async TCP router for DejaVu app plugins.

```text
Server Hub
   ▲
   │ raw TCP, newline-delimited JSON
   │
dejavurouter (one libevent event_base)
   ├── outbound persistent hub connection
   ├── plugin TCP listener 127.0.0.1:39111
   ├── exact route -> plugin registry
   ├── job table + deadlines
   └── response forwarding
```

The server decides the destination with the top-level opaque `route` field.
The router does exact route lookup only:

```text
route -> plugin
```

It does not interpret `method`, `params`, accounts, apps or business rules.
There is no HTTP anywhere.

## Build

```sh
cd dejavurouter
./third_party/fetch-libevent.sh
make
```

ASan/UBSan:

```sh
make asan
JOBS=200 ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
  ROUTER_BIN=$PWD/dejavurouter-asan python3 tests/integration.py
```

## Config

```json
{
  "hub_host": "127.0.0.1",
  "hub_port": 39010,
  "device_id": "phone-a",
  "device_token": "dev-token",
  "listen_host": "127.0.0.1",
  "listen_port": 39111
}
```

`hub_host`/`hub_port` is the raw TCP hub endpoint. The former `hub_url`
HTTP long-poll configuration is gone.

No TLS; network security is intentionally out of scope.

## Hub protocol

Newline-delimited JSON over one persistent outbound TCP connection.

On connect:

```json
{"type":"hello","device_id":"phone-a","version":1,"capabilities":["router.plugin"]}
```

Server request:

```json
{
  "type": "request",
  "id": "job_1",
  "route": "some-opaque-route",
  "method": "anything",
  "params": {},
  "deadline_ms": 30000
}
```

Router response:

```json
{"type":"response","id":"job_1","route":"some-opaque-route","ok":true,"result":{}}
```

Error:

```json
{"type":"response","id":"job_1","route":"some-opaque-route","ok":false,
 "error":{"code":"route_unavailable","message":"no plugin registered for route","retryable":true}}
```

Router reconnects to the hub with exponential backoff. Pending jobs are dropped
on hub disconnect; the server is expected to retry.

### Route announcements

The server must know which account routes are online on which device. The router
announces them over the same TCP connection:

```json
{"type":"route","action":"register","route":"1875541871079874560",
 "device_id":"c44d68aa","app":"com.twitter.android"}
```

```json
{"type":"route","action":"unregister","route":"1875541871079874560"}
```

- plugin hello → one register event per route;
- plugin disconnect → one unregister event per route;
- hub reconnect → a fresh snapshot of currently registered routes;
- hub connection close → server removes the device's runtime routes.

The router does not own the persistent account-to-device mapping. The account
service stores it (account_id, device_id, route); route events provide the live
route -> device connection state.

## Plugin protocol

Plugins connect to `listen_host:listen_port` using raw TCP JSON Lines.

Hello:

```json
{"type":"hello","app":"com.example.demo","plugin":"demo","version":1,
 "routes":["some-opaque-route"],"accounts":[]}
```

Router request:

```json
{"type":"request","id":"job_1","route":"some-opaque-route","method":"anything","params":{}}
```

Plugin response must echo the route:

```json
{"type":"response","id":"job_1","route":"some-opaque-route","ok":true,"result":{}}
```

A response with a missing or mismatched route is rejected as `route_mismatch`.

## Concurrency

- one `event_base`;
- one persistent hub TCP bufferevent;
- one bufferevent per plugin;
- pending jobs keyed by id;
- deadline timers;
- no threads, no blocking calls.

Limits:

```text
line: 1 MiB
max plugins: 512
max in-flight jobs: 8192
```

## Test

```sh
JOBS=500 python3 tests/integration.py
```

### Socket-hiding hub PoC

For server-side tests without touching sockets:

```python
from hub_poc import RouterHub

hub = RouterHub(port=39010).start()
hub.wait_connected(10)
result = hub.call("some-opaque-route", "token", timeout=15)
hub.stop()
```

The caller only sees `call()` / `call_async()` and normal exceptions; TCP
framing, json ids and response correlation are internal.

The PoC server also keeps the persistent account table and live route state
separate:

```python
hub.bind_account("1875541871079874560", "c44d68aa", "1875541871079874560")
result = hub.call_account("1875541871079874560", "token", timeout=15)
```

- `bind_account` is the server-side persistent account/device/route row;
- route register/unregister events maintain the live `hub.routes`;
- `call_account` resolves account -> device -> live connection -> route;
- disconnect clears live routes but keeps the account row;
- after reconnect and route re-announcement, the same account resolves again.

```sh
python3 tools/hub_poc.py --port 39010 --route <route> --method token
python3 tests/test_hub_poc.py
python3 tests/test_hub_poc_e2e.py
```

TCP integration flow:

```text
fake TCP hub -> dejavurouter -> fake plugin
```

## Android

See [android/README.md](android/README.md). The daemon is installed as a Magisk
module, starts on boot, and is restarted by a supervisor after death.