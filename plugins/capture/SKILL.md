---
name: dejavu-plugin-capture
description: Example SSL plaintext capture plugin for Dejavu. Hooks libssl.so SSL_write/SSL_read and appends printable plaintext records to an app files log.
---

# capture plugin

Example **unload-safe** native plugin that captures TLS plaintext at the
BoringSSL boundary:

- resolves `libssl.so!SSL_write` and `libssl.so!SSL_read`;
- installs both hooks through the host `hook` interface;
- appends `OUT len=N ...` / `IN len=N ...` records (first 900 printable bytes)
  to a file under the target app's files dir;
- stops/uninstalls its hooks in `on_unload_prepare`, so `plugin.unload` is safe.

This captures plaintext **after** TLS, so it does not need a proxy or a CA,
and certificate pinning does not affect it. It only covers the process where
the plugin is loaded (usually the app main process).

## Build and install

```sh
./plugins/capture/build.sh
./dejavu plugin -p <pkg> install out/plugins/capture/libdejavu_capture.so \
    --skill plugins/capture/SKILL.md
```

The target app must be in `config/targets.txt` and restart after being added,
so the agent is loaded.

## Use

```sh
CAPTURE=/data/user_de/0/<pkg>/files/dejavu-capture.log
./dejavu plugin -p <pkg> call capture start "$CAPTURE"
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

- `capture.start(path)` requires a writable app-files path;
- `capture.status()` reports `out=<calls>/<bytes>B in=<calls>/<bytes>B`;
- `capture.stop()` uninstalls hooks and closes the file;
- unloading without stop is refused by the host if hooks remain, but
  `on_unload_prepare` normally handles it.
