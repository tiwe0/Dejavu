# capture example plugin

`capture` is an example Dejavu plugin, not part of the framework ABI. It
demonstrates using the host `module` and `hook` services from a native plugin
to capture TLS plaintext at `libssl.so`:

- `capture.start(path[, transport, endpoint])` — resolve
  `SSL_write`/`SSL_read`, install hooks and write records to `path` and/or a
  framework TCP/UDP/KCP stream (`1/2/3`);
- `capture.status()` — call/byte counters;
- `capture.stop()` — uninstall hooks, close the file and stream;
- `capture.probe(transport, endpoint)` — send one test record.

Build:

```sh
./plugins/capture/build.sh
```

Install/use:

```sh
./dejavu plugin -p <pkg> install out/plugins/capture/libdejavu_capture.so \
    --skill plugins/capture/SKILL.md
./dejavu plugin -p <pkg> call capture start --arg s:/data/user_de/0/<pkg>/files/dejavu-capture.log
# trigger traffic
./dejavu plugin -p <pkg> call capture status
./dejavu plugin -p <pkg> call capture stop
./dejavu plugin -p <pkg> unload capture
```

Live stream viewer:

```sh
./dejavu stream --transport tcp --port 39001
./dejavu plugin -p <pkg> call capture start --arg s: --arg 1 --arg s:127.0.0.1:39001
```

For UDP/KCP use `--no-adb-reverse` and `<host-lan-ip>:<port>` as endpoint.
Transport numbers: `1=tcp`, `2=udp`, `3=kcp`.

It is unload-safe: `on_unload_prepare` stops the hooks before module teardown.
Captured logs can contain sensitive tokens; delete them after debugging.
