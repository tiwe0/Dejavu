# X DejaVu plugin design

Working name: `x-agent`.

## Goal

Run one native DejaVu plugin inside each X app instance. The plugin connects
outbound to the local `dejavurouter` over TCP and serves the account's
X-specific RPCs.

The server never talks to the plugin directly. It selects a route, and
`dejavurouter` forwards the route to the plugin.

```text
Server
  │ TCP JSONL, route = account id
  ▼
dejavurouter (root daemon on phone)
  │ exact route lookup
  ▼
X DejaVu plugin inside app
  ├── token
  ├── sign
  └── optional refresh
```

## Route

Route is the only destination selector.

Decision for v1:

```text
route = X account id
```

Example:

```text
1875541871079874560
```

No prefix, no method-based routing, no app-name routing from the server.

Every server request and plugin response echoes the route:

```json
{"type":"request","id":"job_1","route":"1875541871079874560","method":"sign","params":{}}
{"type":"response","id":"job_1","route":"1875541871079874560","ok":true,"result":{}}
```

`dejavurouter` rejects a response whose route is missing or mismatched.

## Plugin hello

On connect to `127.0.0.1:39111`:

```json
{
  "type": "hello",
  "app": "com.twitter.android",
  "plugin": "x-agent",
  "version": 1,
  "routes": ["1875541871079874560"],
  "accounts": ["1875541871079874560"]
}
```

If one X app instance ever holds multiple accounts, the plugin can register
multiple routes. Current deployment has one active account per Android user.

## RPC methods

### token

```json
{"type":"request","id":"j1","route":"187...","method":"token","params":{}}
```

Response:

```json
{"type":"response","id":"j1","route":"187...","ok":true,
 "result":{"token":"<x-attest-token>","expires_at":"...","expires_in":42000}}
```

### sign

```json
{"type":"request","id":"j2","route":"187...","method":"sign",
 "params":{"digest":"<base64 sha256 digest>"}}
```

Response:

```json
{"type":"response","id":"j2","route":"187...","ok":true,
 "result":{"signature":"<base64 DER ECDSA>"}}
```

### refresh

```json
{"type":"request","id":"j3","route":"187...","method":"refresh",
 "params":{"force":true}}
```

Response if actually extended:

```json
{"type":"response","id":"j3","route":"187...","ok":true,
 "result":{"refreshed":true,"token":"...","expires_at":"..."}}
```

If it cannot extend the token, return an error; never return an old token as
successfully refreshed.

## Signer implementation

Preferred design: the plugin owns the X signer code, not a per-account TCP
signer.

The existing `out/clone-bridge-support/src/xsvc_clone.cpp` already contains the
useful pieces:

- AndroidKeyStore key access;
- attestation token cache read;
- JNI glue;
- ECDSA signing.

For the plugin, factor those pieces into:

```text
x-agent/
├── plugin_entry.cpp      DejaVu ABI lifecycle + router client thread
├── router_client.cpp     persistent TCP JSONL client
├── xsigner.cpp           token cache + AndroidKeyStore signing
├── account.cpp           read current_user_id / prefs path
├── json/                 cJSON
└── manifest.json
```

The plugin's app data is the X app's own data, so it can locate:

```text
/data/user/<uid>/com.twitter.android/shared_prefs/
  com.twitter.android_preferences.xml
  hardware_attestation_token_cache.xml
  app_attest_token_cache.xml
```

Using the AndroidKeyStore key directly avoids maintaining mapping from Android
user to xsvc port.

## Refresh boundary

There are two possible layers:

### Option A: plugin only serves token/sign

Refresh is triggered outside the plugin by foregrounding the X app. The plugin
just reads the refreshed cache on the next `token` call.

Advantages: simple, reliable, no background activity hacks.

### Option B: plugin implements refresh

The plugin tries to start the X app activity from its own app context and waits
for the token cache to change. This is best-effort and may be blocked by
Android background-activity restrictions.

Recommendation for v1: implement token and sign first. Add `refresh` as
best-effort only after the plugin is loaded and validated, and keep the external
wake path available.

## Threading

One plugin instance owns one worker thread:

```text
plugin initialize
  → start router thread
  → connect 127.0.0.1:39111
  → send hello
  → read request lines
  → dispatch token/sign
  → send response lines
  → on disconnect: backoff and reconnect
```

No thread per request. Sign operations are serialized inside the plugin because
AndroidKeyStore/signing state is small and per-account.

Lifecycle:

```text
on_control_ready   optional status/log
on_unload_prepare  set stop flag, shutdown socket, join worker
shutdown           final cleanup
```

## Memory safety

- maximum JSON line 1 MiB;
- maximum plugin response 4 MiB;
- cJSON parse with explicit length;
- every allocation checked;
- route string copied with fixed bounds;
- socket buffers bounded;
- no logging of tokens/digests unless debug explicitly enabled;
- one worker thread joined on unload.

## Build

Native arm64 plugin built with the DejaVu plugin ABI:

```text
plugins/x_agent/
├── CMakeLists.txt
├── manifest.json
├── entry.lua              optional control/status helpers
├── plugin_entry.cpp
├── router_client.cpp
├── xsigner.cpp
└── json/
```

Initial scope:

1. ABI lifecycle and router TCP connection;
2. `hello` with route = account id;
3. `token` read;
4. `sign` with AndroidKeyStore;
5. reconnect and line limits;
6. route echo;
7. install/load into one Android user.

Then expand to all 8 users and add refresh handling.

## Install/load

The plugin is loaded by DejaVu inside each X app instance. The same arm64
`.so` can be installed into each Android user's X app files and added to the
allowlist.

The router runs on the phone:

```text
dejavurouter 127.0.0.1:39111
```

No ADB reverse is needed once both are on the phone.

## Open decisions

1. Signer implementation: internal AndroidKeyStore code vs existing per-account
   xsvc ports. Recommendation: internal.
2. Refresh ownership: plugin best-effort vs external foreground wake.
   Recommendation: token/sign first, then test both.
3. Multi-account in one X app: route per account; current deployment one account
   per Android user.
4. Plugin load mechanism: DejaVu plugin allowlist per user, installed by a script
   under `/data/adb/dejavurouter` or app data.
