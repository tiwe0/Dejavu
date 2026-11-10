# X Agent (DejaVu plugin)

Implementation of the `plugins/x_agent/DESIGN.md` design.

## Status

Working for all eight Android X users:

- plugin .so installed in each user's X app files;
- each user's plugin path allowlisted with SHA-256;
- loaded into all eight running X processes through the DejaVu control RPC;
- all eight routes announce on `dejavurouter`;
- all eight token calls succeeded end-to-end.

## Build

```sh
source out/tooling/env.sh
plugins/x_agent/build.sh
```

Output:

```text
out/plugins/x_agent/libdejavu_x_agent.so
```

## Deploy all users

```sh
ADB_SERVER_SOCKET=tcp:127.0.0.1:5037 python3 plugins/x_agent/deploy_all.py
```

The deployer:

1. enumerates `com.twitter.android` PIDs and their Android users;
2. copies the plugin into `/data/user/<user>/com.twitter.android/files/`;
3. sets owner/mode/SELinux label;
4. rebuilds the DejaVu plugin allowlist with SHA-256 hashes;
5. unloads/reloads `x_agent` in every process through a targeted control RPC;
6. starts a local `RouterHub` and verifies all routes plus token RPCs.

It does not force-stop apps, so injected signers remain alive.

## Router test

```sh
su -c 'sh /data/adb/modules/dejavurouter/service.sh'
```

The router's wake target table is installed at:

```text
/data/adb/dejavurouter/wake_targets.json
```

## Next

- duplicate-account / route migration handling;
- production TCP hub consuming route events;
- add a second account in one X app if needed.
