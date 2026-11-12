# Dejavu Router

The framework's internal routing component runs as a separate root daemon and
routes opaque JSON Lines requests between a Hub and local plugins. Manager is a control client: closing the app does not stop the daemon.
The router has no app-specific request methods, account rules or plugin defaults.
Native plugins use the [framework RPC interface](../zygisk/docs/plugin-rpc.md)
for registration and business callbacks; Agent owns the default Unix connection.

## Installation layout

The Dejavu module bundles this internal component and atomically installs its
executable at **`/data/adb/dejavu/bin/dejavuerouter`**. It is not a plugin package
or a separate root module. Its configuration and runtime metadata stay under
`/data/adb/dejavu/router`; Manager continues to use the framework lifecycle entry.
Installation and boot repair the executable without enabling the service.
Module upgrades preserve per-device scopes, plugin trust, startup hooks and
`system.prop`, while updating the framework bootstrap. Unchanged binaries keep their inode; an upgrade preserves the existing process
until it is stopped or restarted. Uninstall stops the component and removes only
its executable, retaining configuration and other tools in `bin/`.

## Manager and lifecycle

Install the rebuilt Dejavu module, then open **Configuration → Router** in Manager.
Start the daemon, expand Hub configuration, and set the Hub host, port, device ID
and optional token. An empty token input retains the existing token. Reconnect
refreshes the Hub connection; it does not replay pending requests.

- **Start** enables supervision and boot startup. A watchdog restarts a crashed daemon.
- **Stop** disables supervision and boot startup, and stops both processes.
- **Online routes** are registered by connected plugins.
- **Wake rules** map a route to an Android user, package and activity. Add/edit/delete
  changes these rules immediately. Deleting a rule does not disconnect an online
  plugin or remove its live route. The optional plugin path is metadata; configure
  injection through the plugin's Scope. Wakeup never overwrites global autoload settings.

The Android root shell must provide `flock` and `setsid` for serialized lifecycle
operations and detached supervision. The module boot script resumes only an explicitly enabled router. A new installation
is stopped by default. Existing standalone `/data/adb/modules/dejavurouter` modules
and `/data/adb/dejavurouter` data are not migrated or stopped automatically. The new module defaults to Unix-only local listeners. Stop the older service
before enabling the same legacy plugin TCP port in this version.

Root lifecycle entry:

```sh
sh /data/adb/modules/dejavu_zygisk/router-control.sh status
sh /data/adb/modules/dejavu_zygisk/router-control.sh start
sh /data/adb/modules/dejavu_zygisk/router-control.sh stop
```

Only configuration and wake rules are persisted in `/data/adb/dejavu/router`.
Forwarded request/response bodies and status history are not saved. Runtime control
metadata contains a fresh management token and is accessible only through root;
Manager caches it in memory. Module removal retains configuration for an explicit
cleanup or reinstall.

## Local transports

On Android, local communication uses named **abstract Unix sockets**:

| Purpose | Default name | Access |
| --- | --- | --- |
| Manager control | `dejavu.router.control` | Root-discovered token on every request; Manager verifies root peer UID |
| Plugin registration and forwarding | `dejavu.router.plugins` | Same JSON Lines protocol as the legacy plugin listener |

Abstract sockets have names without filesystem nodes. On Linux/macOS host builds,
the defaults are filesystem sockets `control.sock` and `plugins.sock` under the
configuration directory. `control_socket` and `plugin_socket` may override the names
at startup, for example in isolated tests. Host overrides are socket basenames
inside the configuration directory; Android overrides are abstract names. The legacy
plugin TCP listener is available on `127.0.0.1:39111` when `listen_tcp: true` is set
in `config.json` before startup. Both module defaults and configurations that omit `listen_tcp` use Unix sockets
only. There is
no management TCP listener. Plugin sockets are for trusted device-local clients;
the separate management token does not authenticate plugin registrations.

Hub transport remains outbound TCP JSON Lines, matching the existing router protocol.
It does not add TLS; use a trusted network or an authenticated encrypted tunnel for
a remote Hub. Hub credentials never appear in status snapshots or Manager diagnostics.

## Protocol

A plugin connects and sends:

```json
{"type":"hello","app":"com.example.demo","version":1,"routes":["demo-route"]}
```

The Hub sends a request. The router replaces its ID with a unique transport ID;
the plugin echoes that transport ID and route in its response. The router restores
the original ID before sending the response to the Hub:

```json
{"type":"request","id":"job-1","route":"demo-route","method":"echo","params":{},"deadline_ms":30000}
{"type":"request","id":"rpc-1","route":"demo-route","method":"echo","params":{},"deadline_monotonic_ms":123456789}
{"type":"response","id":"rpc-1","route":"demo-route","ok":true,"result":{}}
{"type":"response","id":"job-1","route":"demo-route","ok":true,"result":{}}
```

Transport IDs are not reused during a daemon's lifetime. A late response cannot
complete another request that reuses the original Hub ID after cancellation,
timeout or reconnect. Requests are validated before dispatch; encoded wire lines
are limited to 1 MiB, including added fields and restored IDs. Oversized final
Hub responses return `response_too_large` without closing healthy services.
Plugin connections must finish hello within five seconds of connecting; sending
partial input does not extend this deadline.

Route announcements use `type: "route"`, `action: "register" | "unregister"`.
A Hub reconnect announces the current online routes again. In-flight jobs are
removed on disconnect and are never automatically replayed. A Hub `wake` request
uses a configured rule to launch the explicit Android activity and waits for the
route to register, subject to its timeout.

`control.json` publishes `transport`, `namespace`, `name` and `token`. Each local
management request is a single JSON line with `token`, `op` and operation fields:

| Operation | Fields |
| --- | --- |
| `status` | None |
| `reconnect` | None |
| `configure` | `hub_host`, `hub_port`, `device_id`, optional `device_token` |
| `route_put` | `route`, `user_id`, `package`, `activity`, optional `plugin_path` |
| `route_delete` | `route` |

Success returns `{"ok":true,"status":{...}}`; failure returns
`{"ok":false,"error":"stable_code"}`. Snapshots include Hub state, counts,
non-secret connection settings and merged online/configured routes. Controls have
bounded connections, input sizes and timeouts. Configurations and rules are
validated before an atomic replacement; credentials are not returned by the API.

## Build and verification

CMake 3.28+, Ninja and a C compiler are required. Android additionally needs NDK r29.
The build fetches libevent 2.1.12-stable with SHA-256 verification; cJSON 1.7.19 is
vendored. Generated files stay under the project `out/` directory.

```sh
router/scripts/build.sh host
router/scripts/build.sh android
# The regular Zygisk build/package also includes the independent executable.
make -C zygisk package
```

A host-provided libevent can be selected with
`router/scripts/build.sh host -DDEJAVU_ROUTER_SYSTEM_LIBEVENT=ON`.

```sh
ROUTER_BIN="$PWD/out/router-host/dejavuerouter" python3 router/tests/integration.py
python3 router/scripts/test-install.py
python3 router/scripts/test-lifecycle.py
# Isolated Android process, ports and socket names; no installed-service or UI changes.
python3 router/tests/device_smoke.py --adb /path/to/adb --serial SERIAL \
  --binary out/router-android-arm64/dejavuerouter
```

Lifecycle identity tests require Linux `/proc`; macOS reports that test as skipped.
Android deployment and upgrade checks use an isolated internal directory layout:

```sh
python3 router/tests/device_lifecycle.py --adb /path/to/adb --serial SERIAL \
  --binary out/router-android-arm64/dejavuerouter
```

For the production Manager client's Android UID/SELinux transport check, build
`app/tests/router/device/build.sh`, install its APK and the side-by-side Manager,
then add `--instrumentation cafe.ivory.dejavu.routersmoke/dejavu.tests.RouterAndroidInstrumentation`
to the device smoke command. This supplies a temporary private endpoint fixture;
it verifies the real socket client, peer checks and caching, and does not exercise
Manager root authorization. `metadata_path` instrumentation mode tests root
discovery separately on a Manager installation that already has root permission.

The Android smoke uses a synthetic Hub/plugin, removes only its own temporary
files/ADB mappings and leaves the installed router and target apps running.
