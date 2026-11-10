# Android deployment

`dejavurouter` is installed as a Magisk module under:

```text
/data/adb/modules/dejavurouter/
/data/adb/dejavurouter/dejavurouter
/data/adb/dejavurouter/config.json
```

## Build

```sh
./android/build.sh
```

This cross-compiles libevent (static) and the router for aarch64 Android.

## Install

```sh
ADB_SERVER_SOCKET=tcp:127.0.0.1:5037 ./android/install.sh
```

The installer:

- pushes the binary and module files;
- preserves an existing `/data/adb/dejavurouter/config.json`;
- starts the supervisor immediately;
- installs the Magisk module for boot startup.

## Boot and restart

- `post-fs-data.sh` clears stale PID/lock state at boot;
- `service.sh` starts the supervisor at late_start;
- `supervisor.sh` restarts `dejavurouter` whenever the process exits;
- `watchdog.log` records starts/exits;
- `router.log` records daemon output.

Check:

```sh
su -c 'ps -A -o PID,USER,NAME | grep dejavurouter'
su -c 'ss -ltn | grep 39111'
su -c 'tail -20 /data/adb/dejavurouter/watchdog.log'
```

## Config

Edit:

```sh
su -c 'vi /data/adb/dejavurouter/config.json'
```

Then:

```sh
su -c 'kill $(cat /data/adb/dejavurouter/supervisor.pid)' 2>/dev/null || true
su -c 'sh /data/adb/modules/dejavurouter/service.sh'
```

The default `hub_host`/`hub_port` is a local development placeholder. Set it
to the real raw-TCP hub (network security is intentionally out of scope).

Plugins in apps connect to:

```text
127.0.0.1:39111
```

## Self-restart verification

Kill the daemon process only:

```sh
su -c 'pkill -9 -x dejavurouter'
sleep 3
su -c 'pgrep -a dejavurouter'
su -c 'tail -5 /data/adb/dejavurouter/watchdog.log'
```

The supervisor should restart it with a new PID.