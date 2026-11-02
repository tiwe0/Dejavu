# Example plugins

These shared objects are **examples and ABI contract tests**, not part of the
framework. They are built separately from the agent and loaded through
`plugin.load()` / `debug.dejavu.plugins`.

- [`capture/`](capture/) — example SSL plaintext capture plugin using the host
  module+hook interfaces.
- [`algorithm/`](algorithm/) — algorithm self-disclosure tracer: native libcrypto
  hooks plus a Java crypto Lua tracer.
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
./plugins/capture/build.sh
./plugins/unpack/build.sh
./plugins/package.sh
```

Each plugin may ship a `SKILL.md` next to its source. At install time copy it
next to the loaded library as `<plugin>.so.SKILL.md` (or a shared
`SKILL.md` in the same directory); the running plugin then exposes it through
`plugin.skill(name)` and `./dejavu plugin skill <name>` so an agent can read the
plugin-specific instructions without guessing the repo path.

Example plugins are built separately from the module. `plugins/package.sh`
creates `dejavu-plugins-vX.Y.Z-arm64.zip` containing the native `.so` files,
their SKILL sidecars, JIT examples and `unpack/cli.py`.
Every plugin has its own folder and `manifest.json` declaring its identifier,
display name, version, description, kind, entry and resource files. The app only
recognizes manifest folders directly under `/data/adb/dejavu/plugins`; bare
`.so`/`.c` files and folders without manifests are ignored. Run
`python3 plugins/prepare-library.py` after native builds to assemble the seven
importable folders under `out/plugins`. Import an individual folder in the app,
or copy these folders to the default device library and scan. Import and scan
do not execute plugins; native/JIT loading deploys and hash-pins only the selected
plugin for the selected target.

CI uploads the zip as the
`dejavu-example-plugins` artifact and attaches the zip to the GitHub Release as
a second asset; the module zip remains framework-only.

The ABI and host services are documented in
[`zygisk/docs/plugin-api.md`](../zygisk/docs/plugin-api.md).
