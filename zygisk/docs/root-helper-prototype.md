# Root helper (production service)

On Android 15, APEX `libcrypto.so` text can be **execute-only** (`--xp`).
Dobby must read the target prologue for relocation, so an in-app agent armed
with only the app UID cannot inline-hook those functions.

The module ships a separate root helper that is started by `service.sh` at
boot and managed by Magisk/KernelSU:

```text
root helper (adb su / Magisk service)
  │  ptrace target, inject mprotect
  ▼
target process (app UID)
  agent → root_helper_unlock() → Dobby
```

## Build

```sh
zygisk/scripts/build-root-helper.sh
# zygisk/out/root-helper/dejavu-root-helper
```

The helper is a separate arm64 executable; it is **not** part of the app
process and must run as root.

## Production startup

`zygisk/module/service.sh` (packaged into the module) starts a watchdog in the
background. The helper:

- binds an ephemeral `127.0.0.1` port;
- generates a random 256-bit token;
- publishes `debug.dejavu.root.port` / `debug.dejavu.root.token`;
- only accepts mprotect requests for process names listed in
  `config/targets.txt`;
- only allows executable mappings and read/write/exec protections;
- is restarted by the watchdog if it exits.

`zygisk/module/uninstall.sh` stops the watchdog/helper and clears properties.

## Manual verification (without reboot)

```sh
adb shell su -c 'sh /data/adb/modules/dejavu_zygisk/service.sh'
adb shell su -c 'getprop debug.dejavu.root.port; getprop debug.dejavu.root.token'
```

The helper runs as root; the in-app agent never gains root itself.

The agent reads those two properties, connects to `127.0.0.1:<port>` and asks
the helper to temporarily `mprotect` the mapping containing the hook target.
The helper attaches with `ptrace`, injects `mprotect` using a `svc #0` gadget,
returns the syscall result, restores the target registers and detaches.

## Verified behavior

With `plugins/algorithm` on Android 15 / `com.android.settings`:

- `algorithm.start` installed **16 libcrypto hooks** without crashing;
- `algorithm.selftest` produced:
  - `DIGEST_INIT`
  - `DIGEST_UPDATE ... data=616263`
  - `DIGEST_FINAL ... data=900150983cd24fb0d6963f7d28e17f72`
- target process stayed alive.

## Security model

- localhost-only TCP, random per-start token;
- target process allowlist from `config/targets.txt`;
- request validation: executable mapping only, page-aligned range, allowed
  protection bits;
- no arbitrary read/write RPC — only temporary `mprotect`;
- Magisk service + uninstall cleanup; properties cleared on stop.

Root helper still has broad power by design. Treat the module and token as
sensitive; do not expose the helper port outside localhost.

## Verified

- production `service.sh` starts helper with dynamic port/token;
- invalid token rejected (`ERR token`);
- non-target PID rejected (`ERR target is not allowlisted`);
- algorithm plugin via root helper: 15 libcrypto hooks + EVP digest selftest;
- native `native.install` path also unlocks libcrypto and uninstalls cleanly.
