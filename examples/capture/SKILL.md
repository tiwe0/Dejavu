---
name: dejavu-plugin-capture
description: Example SSL plaintext capture plugin for Dejavu. Hooks libssl.so SSL_write/SSL_read and appends printable plaintext records to an app files log.
---

# capture plugin

Example **unload-safe** native plugin that captures TLS plaintext at the
BoringSSL boundary:

- resolves `libssl.so!SSL_write` and `libssl.so!SSL_read`;
- installs both hooks through the host `hook` interface;
- writes `OUT len=N ...` / `IN len=N ...` records (first 900 printable bytes)
  to a file and/or a framework **TCP/UDP/KCP stream**;
- attempts to stop capture and remove hooks in `on_unload_prepare`.

The stream interface is framework-level: any plugin declaring
`DEJAVU_PLUGIN_CAP_STREAM` can open a TCP/UDP/KCP stream and write records;
`capture` is only a consumer/example.

This captures plaintext **after** TLS, so it does not need a proxy or a CA,
and certificate pinning does not affect it. It only covers the process where
the plugin is loaded (usually the app main process).

## Live stream

Terminal A (host viewer):

```sh
# TCP: the CLI sets up adb reverse automatically.
./dejavu stream --transport tcp --port 39001

# UDP/KCP: device and host must be on the same network; use the host LAN IP.
./dejavu stream --transport udp --port 39001 --no-adb-reverse
./dejavu stream --transport kcp --port 39001 --no-adb-reverse
```

Terminal B:

```sh
# file only
./dejavu plugin -p <pkg> call capture start --arg s:/data/user_de/0/<pkg>/files/capture.log
# TCP stream with discarded file output
./dejavu plugin -p <pkg> call capture start --arg s:/dev/null --arg 1 --arg s:127.0.0.1:39001
# UDP stream (host LAN IP)
./dejavu plugin -p <pkg> call capture start --arg s:/dev/null --arg 2 --arg s:<host-lan-ip>:39001
# KCP stream
./dejavu plugin -p <pkg> call capture start --arg s:/dev/null --arg 3 --arg s:<host-lan-ip>:39001
```

Transport numbers: `1=tcp`, `2=udp`, `3=kcp`. `capture.probe <transport>
<endpoint>` writes a single `dejavu-stream-probe` record to validate a stream
without needing app traffic.

## Build and install

```sh
./examples/capture/build.sh
./dejavu plugin -p <pkg> install out/plugins/capture/libdejavu_capture.so \
    --skill examples/capture/SKILL.md
```

The target app must be in `config/targets.txt` and restart after being added,
so the agent is loaded.

## Use

```sh
CAPTURE=/data/user_de/0/<pkg>/files/dejavu-capture.log
./dejavu plugin -p <pkg> call capture start --arg "s:$CAPTURE"
# trigger app network traffic (foreground refresh, login, etc.)
./dejavu plugin -p <pkg> call capture status
./dejavu plugin -p <pkg> call capture stop
./dejavu plugin -p <pkg> unload capture
adb -s "$ADB_SERIAL" shell "su -c 'cat $CAPTURE'" | head
```

Remove the capture file afterwards; it may contain cookies/tokens or personal
data. The plugin only captures first 900 bytes per SSL call and does not
decode HTTP/2, gzip or binary frames.

## Agent notes

- `capture.start(path)` opens a writable file sink. Use `/dev/null` to discard
  file output when streaming. The current record callback returns early if
  that descriptor is closed, so an empty path skips records and counters even
  when the stream connects. [Record callback](plugin_entry.cpp)
- `capture.status()` reports `out=<calls>/<bytes>B in=<calls>/<bytes>B`;
- `capture.stop()` uninstalls hooks, closes the file and stream;
- `capture.probe(transport, endpoint)` sends a one-record test;
- `plugin.unload` calls `on_unload_prepare` and the host attempts to remove any
  remaining tracked hooks. Failed cleanup leaves the module loaded for retry.
