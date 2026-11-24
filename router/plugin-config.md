# Plugin routing configuration

The independent Router daemon reads routing metadata from installed plugin
folders. Lua and native plugins still register with the same local Unix socket;
they do not create upstream connections or handle Hub credentials.

## Package declaration

Add `router` to the plugin's schema-v1 `manifest.json` and declare the resource
in `files`:

```json
{
  "schemaVersion": 1,
  "id": "example",
  "name": "Example",
  "version": "1.0.0",
  "kind": "lua",
  "entry": "main.lua",
  "router": "router.json",
  "files": ["main.lua", "router.json"]
}
```

The same declaration works for native and JIT packages. The filename must be
`router.json`, distinct from the entry and page resource. Manager validates it
before import. It is daemon metadata and is omitted from the injected Lua
resource bundle. A plugin with no `router` declaration continues using the
default Hub, even if an undeclared `router.json` happens to exist beside it.

## Routing file

```json
{
  "schemaVersion": 1,
  "services": ["example_business"],
  "upstreams": [
    {
      "id": "main",
      "host": "hub.example.com",
      "port": 39010,
      "protocol": "v2",
      "tls": true,
      "tls_sni": "hub.example.com"
    },
    {"id": "secondary", "host": "other.example.com", "port": 39010, "protocol": "v1"}
  ],
  "default_upstream": "main",
  "routes": {
    "example.secondary_account": "secondary"
  }
}
```

Each upstream entry supports:

| Field | Required | Meaning |
| --- | --- | --- |
| `id` | yes | Local upstream ID (`[a-z][a-z0-9_-]{0,63}`) |
| `host`, `port` | yes | Hub hostname/IP (`<=253` bytes) and port (`1..65535`) |
| `device_id` | no | Overrides the default device ID (`<=128` bytes) |
| `device_token` | no | Upstream credential (`<=4096` bytes; defaults to empty, never inherited) |
| `protocol` | no | `"v1"` (default, TCP JSON Lines) or `"v2"` (binary framed Router v2) |
| `tls` | no | Boolean; enables mbedTLS transport to the Hub |
| `tls_ca_file` | no | Absolute path (`<=1024` bytes) to a PEM CA bundle for TLS verification |
| `tls_sni` | no | Optional TLS SNI override (`<=253` bytes; defaults to `host`) |
| `tls_insecure` | no | Boolean; skips certificate verification (testing only) |

`services` binds the file to `rpc.register {name = "example_business", ...}`
or native `DejavuRpcServiceDescriptor.service_name`, which the framework sends
as `hello.plugin`. When omitted, it defaults to the manifest ID. It is separate
from the package name, app package and account route. Services must be unique
across configured plugins. Legacy clients without `hello.plugin` use the
default Hub for unassigned routes. An explicitly assigned route can only be
registered by a matching service; clients using that route must send the
matching `hello.plugin` value. A conflicting reload is rejected atomically.

Each registered route goes to exactly one upstream: an explicit `routes`
assignment takes precedence, otherwise the owning service's `default_upstream`
applies. Routes belonging to legacy services use the default Hub. An upstream
receives only its assigned route announcements and can request or wake only
those routes. Different plugins may use different Hubs concurrently; one
plugin may split its routes across several Hubs. Requests are never broadcast
or replayed to another Hub.

Declare a custom route in `routes` when its Hub needs to wake an offline
plugin: before registration, Router cannot infer which service owns a dynamic
account route. Wake rules still select the local Android user, package and
activity; routing metadata does not grant injection scope or launch arbitrary
Hub-supplied targets.

Upstream IDs are local to a plugin. Management identifies them as
`<manifest.id>/<upstream.id>`, such as `example/main`; `default` is reserved for
the existing default Hub. An optional upstream `device_id` replaces the default
device identity. `device_token` defaults to empty; plugin upstreams never inherit
the default Hub's credential. Avoid shipping real
tokens in distributable ZIPs; configure device credentials through Manager.

Limits are 32 KiB per routing file, eight upstreams and eight services per
plugin, 32 total upstreams including the default, and 64 configured plugin
folders. Service names use `[A-Za-z0-9_.-]{1,128}`. Local upstream IDs use
`[a-z][a-z0-9_-]{0,63}`. A routing file may assign up to 512 distinct routes;
route names are nonempty text of at most 128 UTF-8 bytes without control
characters. Hosts are nonempty and at most 253 bytes, ports are integers from
1 through 65535, device IDs are nonempty and at most 128 bytes, and tokens are
at most 4096 bytes. References must name a declared upstream. Duplicate JSON
keys, trailing data and invalid declarations are rejected. The configured plugin
root, plugin folders and their metadata files must not themselves be symbolic
links; the configured root's parent path is trusted.

## Discovery and local overrides

On Android, Router scans `/data/adb/dejavu/plugins` at startup. Host builds
default to a `plugins` directory beside `config.json`. The daemon's
`plugins_dir` setting can select an absolute directory for isolated tests or
custom installations. Discovery reads directory resources only and does not
query injected processes or execute plugin code. Closing Manager does not
affect discovery or upstream connections.

After importing, replacing or deleting a plugin, use **Configuration → Router →
Reload plugin configurations** to apply the directory snapshot. Reload validates
the complete candidate before applying it. An invalid reload leaves the previous
configuration active; an invalid initial configuration prevents daemon startup.
Unchanged upstreams keep their connections. Removed or changed upstreams cancel
their affected work rather than replaying it.

Manager lists the default Hub and each plugin upstream with independent
connection status and configuration source. Editing a plugin upstream saves a
device-local override in `/data/adb/dejavu/router/config.json`; resetting the
override restores the package settings. Overrides do not rewrite plugin files.
An empty token input keeps the current credential. Tokens are never returned in
status snapshots. Only configuration and wake rules persist, not forwarded
request/response bodies or history.

Each upstream has its own connection, DNS resolution and reconnect state.
Requests and wake IDs are scoped to their source upstream. Identical request
IDs on two Hubs are independent. Disconnecting one Hub cancels only its work;
late replies cannot complete requests on another connection.

The local plugin listener remains a trusted-device interface. Service-name and
route binding select routing policy; they are not cryptographic plugin identity
or an authentication boundary between untrusted local plugins. Hub transport
supports both legacy TCP JSON Lines (`protocol: "v1"`) and binary-framed
Router v2 (`protocol: "v2"`), with optional mbedTLS encryption (`tls: true`)
and route telemetry / heartbeat liveness monitoring (`hub_protocol`, `hub_tls`,
`hub_tls_version`, `last_error`, `last_rtt_ms`, `last_heartbeat_ack_ms`,
`reconnect_count` in the control status snapshot). For plaintext v1 or
non-TLS upstreams, use a trusted network or an encrypted tunnel.
