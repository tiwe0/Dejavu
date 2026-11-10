# dejavurouter

A small C device router for DejaVu app plugins.

```text
Server Hub
   ▲ HTTP long-poll (router initiates)
   │
dejavurouter (host or Android root daemon)
   ├─ listens on 127.0.0.1:39111 for DejaVu plugins
   ├─ keeps route -> plugin registry
   ├─ forwards jobs selected by the server's top-level route field
   └─ runs the configured foreground-wake command when a plugin is offline
```

The server is intentionally not modified by this project. The router is generic:
it does not contain X account logic. App-specific work belongs in DejaVu plugins;
the router only handles connections, routes, wake, deadlines and retries.

## Build

```sh
make
./dejavurouter config.json
```

The current build is a Linux host build with plain HTTP hub transport. TLS is
terminated by a local proxy/cloudflared in front of the hub; the next milestone
is an Android NDK build with mbedTLS or a local cloudflared sidecar.

## Configuration

```json
{
  "hub_url": "http://127.0.0.1:39010",
  "device_id": "phone-a",
  "device_token": "dev-token",
  "listen_host": "127.0.0.1",
  "listen_port": 39111,
  "plugin_token": "dev-plugin-token",
  "poll_wait_seconds": 25,
  "plugin_wait_seconds": 20,
  "routes": {
    "demo": {
      "app": "com.example.demo",
      "wake": "adb -s c44d68aa shell am start --user 0 -n com.example.demo/.MainActivity",
      "plugin_wait_seconds": 20
    }
  }
}
```

## Plugin protocol

Newline-delimited JSON over TCP.

Hello:

```json
{"type":"hello","token":"dev-plugin-token","app":"com.example.demo","plugin":"demo","version":1,"routes":["demo"],"accounts":[]}
```

Request from router:

```json
{"type":"request","id":"rpc_1_...","route":"demo","method":"demo.action","params":{}}
```

Response:

```json
{"type":"response","id":"rpc_1_...","ok":true,"result":{"value":1}}
```

When the plugin runs in an app on the phone and the router runs on the host,
connect them with ADB reverse:

```sh
adb -s c44d68aa reverse tcp:39111 tcp:39111
```

## Server job

The hub adds a top-level `route` field:

```json
{
  "id": "job_1",
  "route": "demo",
  "method": "demo.action",
  "params": {"account_id": "..."},
  "deadline_ms": 45000
}
```

If no route is present, the router derives it from the method prefix.

## Status

- Working on host:
  - outbound hub hello/poll/result;
  - plugin listener, registration and route dispatch;
  - plugin timeout and unauthorized rejection;
  - wake command before waiting for a plugin.
- Not yet:
  - Android NDK build;
  - TLS hub transport;
  - systemd/Magisk packaging;
  - full DejaVu X plugin.

Run the integration check with:

```sh
python3 tests/integration.py
```
