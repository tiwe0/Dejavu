# dejavurouter

A small, single-threaded async device router for DejaVu app plugins.

v2 is a libevent event loop, designed for high concurrency and simple
two-end forwarding:

```text
Server Hub (HTTP long-poll)
        ▲
        │ multiple async pollers
        │
  dejavurouter (one event_base)
        ├── plugin TCP listener 127.0.0.1:39111
        ├── route -> plugin registry
        ├── job table + deadlines
        └── async result POST
```

The server decides the destination with an opaque top-level `route` parameter.
The router only does exact route lookup and forwards the request/response. It
does not inspect `method`, `params`, accounts, apps or business rules.

## Build

```sh
cd dejavurouter
./third_party/fetch-libevent.sh
make
```

For an instrumented build:

```sh
make asan
```

## Run

```sh
cp config.example.json config.json
./dejavurouter config.json
```

Config:

```json
{
  "hub_url": "http://127.0.0.1:39010",
  "device_id": "phone-a",
  "device_token": "dev-token",
  "listen_host": "127.0.0.1",
  "listen_port": 39111,
  "pollers": 8,
  "poll_wait_seconds": 25
}
```

No TLS; network security is intentionally out of scope. The hub URL must be
plain HTTP (for example a local proxy or a development hub).

## Hub job

The server includes an opaque route:

```json
{
  "id": "job_1",
  "route": "some-opaque-route",
  "method": "anything",
  "params": {},
  "deadline_ms": 30000
}
```

- Missing/unknown route: `route_unavailable`.
- No method-prefix inference, fallback or per-app routing.
- `method` and `params` are forwarded as-is.

## Plugin connection

Newline-delimited JSON over TCP to `listen_host:listen_port`.

Hello:

```json
{"type":"hello","app":"com.example.demo","plugin":"demo","version":1,"routes":["some-opaque-route"],"accounts":[]}
```

Router request:

```json
{"type":"request","id":"job_1","route":"some-opaque-route","method":"anything","params":{}}
```

Plugin response:

```json
{"type":"response","id":"job_1","ok":true,"result":{}}
```

Error:

```json
{"type":"response","id":"job_1","ok":false,"error":{"code":"not_ready","message":"..."}}
```

For a plugin running inside an app on the phone while the router runs on the
host, reverse the port:

```sh
adb -s c44d68aa reverse tcp:39111 tcp:39111
```

## Concurrency model

- one `event_base`;
- configurable number of async hub long-poll requests;
- one bufferevent per plugin;
- pending jobs keyed by server job id;
- per-job deadline timers;
- async result POST;
- no thread per plugin, no blocking plugin calls.

Limits:

```text
plugin line: 1 MiB
max plugins: 512
max in-flight jobs: 8192
max hub response: 4 MiB
```

## Test

```sh
JOBS=50 python3 tests/integration.py
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
  ROUTER_BIN=$PWD/dejavurouter-asan JOBS=50 python3 tests/integration.py
```

The test runs a mock hub, a fake DejaVu plugin, and the C router.

## Status

Working:

- async libevent event loop;
- exact-route plugin registry;
- bidirectional forwarding;
- job deadlines and plugin disconnect handling;
- async hub pollers and async result posts;
- 50-job integration test and ASan run.

Not included by design:

- TLS/network security;
- authentication;
- method/params validation;
- wake commands;
- complex routing.

Legacy v1 thread implementation remains available as:

```sh
make legacy
```
