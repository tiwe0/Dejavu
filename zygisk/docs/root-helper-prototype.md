# Root helper prototype

On Android 15, APEX `libcrypto.so` text can be **execute-only** (`--xp`).
Dobby must read the target prologue for relocation, so an in-app agent armed
with only the app UID cannot inline-hook those functions.

This prototype adds a separate root helper:

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

## Start (prototype)

```sh
adb push zygisk/out/root-helper/dejavu-root-helper /data/local/tmp/
adb shell su -c '
  chmod 755 /data/local/tmp/dejavu-root-helper
  nohup /data/local/tmp/dejavu-root-helper --port 39401 --token rootproto \
    >/data/local/tmp/root-helper.log 2>&1 </dev/null &
  setprop debug.dejavu.root.port 39401
  setprop debug.dejavu.root.token rootproto
'
```

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

## Limits / security

- Prototype, not production: token is a simple shared string, localhost TCP.
- Root helper can modify arbitrary process memory; keep it root-only, do not
  expose beyond localhost, and remove the properties when done.
- Future work: package it via Magisk `service.sh`, add peer credential checks,
  restrict PID/target allowlist, and integrate with native hooks as well.
