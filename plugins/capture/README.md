# capture example plugin

`capture` is an example Dejavu plugin, not part of the framework ABI. It
demonstrates using the host `module` and `hook` services from a native plugin
to capture TLS plaintext at `libssl.so`:

- `capture.start(path)` — resolve `SSL_write`/`SSL_read`, install hooks and
  append records to `path`;
- `capture.status()` — call/byte counters;
- `capture.stop()` — uninstall hooks and close the file.

Build:

```sh
./plugins/capture/build.sh
```

Install/use:

```sh
./dejavu plugin -p <pkg> install out/plugins/capture/libdejavu_capture.so \
    --skill plugins/capture/SKILL.md
./dejavu plugin -p <pkg> call capture start /data/user_de/0/<pkg>/files/dejavu-capture.log
# trigger traffic
./dejavu plugin -p <pkg> call capture status
./dejavu plugin -p <pkg> call capture stop
./dejavu plugin -p <pkg> unload capture
```

It is unload-safe: `on_unload_prepare` stops the hooks before module teardown.
Captured logs can contain sensitive tokens; delete them after debugging.
