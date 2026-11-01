# Example plugins

These shared objects are **examples and ABI contract tests**, not part of the
framework. They are built separately from the agent and loaded through
`plugin.load()` / `debug.dejavu.plugins`.

- [`hello/`](hello/plugin_entry.cpp) — minimal unload-safe plugin that
  exercises host services (`mem.query`, `module.resolve`), registration and
  safe unload. Built by `hello/build.sh`.
- [`unpack/`](unpack/README.md) — larger DEX capture/active-invocation example.
  Not unload-safe because it installs native ART hooks; it uses the host hook
  interface instead of linking Dobby directly. Built by `unpack/build.sh` and
  driven by `unpack/cli.py`.

Build both:

```sh
./plugins/hello/build.sh
./plugins/unpack/build.sh
```

The ABI and host services are documented in
[`zygisk/docs/plugin-api.md`](../zygisk/docs/plugin-api.md).
