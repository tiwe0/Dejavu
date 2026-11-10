# X Agent (DejaVu plugin)

Implementation of the `plugins/x_agent/DESIGN.md` design.

## Status

Working for one Android user (X account route verified):

- loads inside `com.twitter.android`;
- detects the current X account from app shared preferences;
- connects to `127.0.0.1:39111`;
- registers route = account id;
- serves `token` and `sign`;
- signs with AndroidKeyStore alias:
  `twitter-android-keystore-alias-<account>`;
- returns `refresh_not_supported` for `refresh` (refresh is owned by the
  external foreground wake path).

End-to-end test result:

```text
router TCP hello: ok
token response: ok, route echoed
sign response: ok, route echoed
ECDSA signature verified against the token publicKey
```

## Build

```sh
source out/tooling/env.sh
plugins/x_agent/build.sh
```

Output:

```text
out/plugins/x_agent/libdejavu_x_agent.so
```

## Install one user

The current DejaVu CLI loads into the first process it finds, so target the
directory of that process's Android user:

```sh
./dejavu -s c44d68aa plugin -p com.twitter.android install \
  --dir /data/user/14/com.twitter.android/files \
  out/plugins/x_agent/libdejavu_x_agent.so
```

Replace `14` with the Android user that owns the selected X process.

## Router test

The router must be running on the device:

```sh
su -c 'sh /data/adb/modules/dejavurouter/service.sh'
```

A TCP hub must be reachable at the router's configured `hub_host:hub_port`.
For a host-side test, map the port with:

```sh
adb -s c44d68aa reverse tcp:39010 tcp:39010
```

Then run a TCP JSONL hub and send:

```json
{"type":"request","id":"token1","route":"<account>","method":"token","params":{}}
```

## Next

- multi-user install/load for all X app instances;
- per-PID DejaVu control loading (the CLI picks one process);
- route health/status method;
- external foreground wake for refresh.
