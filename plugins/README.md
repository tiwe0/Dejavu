# Example plugins

These shared objects are **examples and ABI contract tests**, not part of the
framework. They are built separately from the agent and loaded through
`plugin.load()` / `debug.dejavu.plugins`.

- [`hello/device-test.sh`](hello/device-test.sh) — automated on-device
  native/JIT lifecycle smoke test (`make plugin-device-test`).
- [`hello/hello_fs_jit.c`](hello/hello_fs_jit.c) — JIT plugin exercising the
  host fs interface (read/write/exists/remove) without libc.
- [`hello/hello_hook_jit.c`](hello/hello_hook_jit.c) — JIT plugin installing a
  host-tracked `atoi` hook and relying on host teardown at unload.
- [`hello/hello_jit.c`](hello/hello_jit.c) — TinyCC JIT version: `plugin jit`
  reads/compiles the C source in-process; exercises host mem/module services.
- [`hello/`](hello/plugin_entry.cpp) — minimal unload-safe plugin that
  exercises host services (`mem.query`, `module.resolve`), registration and
  safe unload. Built by `hello/build.sh`.
- [`unpack/`](unpack/README.md) — larger DEX capture/active-invocation example.
  Not unload-safe because it installs native ART hooks; it uses the host hook
  interface instead of linking Dobby directly. Built by `unpack/build.sh` and
  driven by `unpack/cli.py`.

One-stop install (push + permissions + allowlist + load):

```sh
./dejavu plugin -p <pkg> install out/plugins/hello/libdejavu_hello.so \
    --skill plugins/hello/SKILL.md
./dejavu plugin -p <pkg> install plugins/hello/hello_jit.c
```

Build both:

```sh
./plugins/hello/build.sh
./plugins/unpack/build.sh
```

Each plugin may ship a `SKILL.md` next to its source. At install time copy it
next to the loaded library as `<plugin>.so.SKILL.md` (or a shared
`SKILL.md` in the same directory); the running plugin then exposes it through
`plugin.skill(name)` and `./dejavu plugin skill <name>` so an agent can read the
plugin-specific instructions without guessing the repo path.

Example plugins are built separately from the module and uploaded as the
`dejavu-example-plugins` CI artifact; they are **not** included in the release
zip (the module remains framework-only).

The ABI and host services are documented in
[`zygisk/docs/plugin-api.md`](../zygisk/docs/plugin-api.md).
