---
name: dejavu-plugin-algorithm
description: Example algorithm self-disclosure plugin. Traces native libcrypto digest/HMAC/cipher/sign operations and includes a Java crypto Lua tracer.
---

# algorithm plugin

Independent algorithm tracer, not tied to any target app.

## Native layer (`algorithm` plugin)

The native plugin hooks exported `libcrypto.so` functions through the host
`module` and `hook` services:

- EVP digest init/update/final
- HMAC init/update/final (including HMAC key)
- EVP encrypt/decrypt init/update/final (including cipher key/IV)
- EVP digest sign/verify update/final

It reports the operation (`DIGEST_/HMAC_/CIPHER_/SIGN_/VERIFY_`), context
pointer, algorithm name when the BoringSSL version exposes it, input/output
hex (first 1024 bytes), and keys/IVs where reachable.

## Java layer (Lua + Java hooks)

`java/algorithm_java.lua` uses the existing Java hook API to trace:

- `MessageDigest.digest/update`
- `Mac.doFinal/update`
- `Cipher.doFinal/update`

It calls `getAlgorithm()` and uses `Arrays.toString` to log input/output byte
arrays to the `DejavuHook` logcat tag.

## Build

```sh
./plugins/algorithm/build.sh
```

## Install / start native tracing

```sh
CAPTURE=/data/user_de/0/<pkg>/files/algorithm.log
./dejavu plugin -p <pkg> install out/plugins/algorithm/libdejavu_algorithm.so \
    --skill plugins/algorithm/SKILL.md
./dejavu plugin -p <pkg> call algorithm start --arg "s:$CAPTURE"
# or live stream (framework TCP/UDP/KCP):
# ./dejavu plugin -p <pkg> call algorithm start --arg s: --arg 1 --arg s:127.0.0.1:39001
./dejavu plugin -p <pkg> call algorithm status
./dejavu plugin -p <pkg> call algorithm stop
./dejavu plugin -p <pkg> unload algorithm
```

If the target has not loaded libcrypto yet, open/use a crypto API once first.
On Android 15 some APEX BoringSSL libraries have **execute-only** text; Dobby
cannot relocate those functions, so the host now refuses the hook instead of
crashing and the plugin reports "no libcrypto algorithm symbols were
hookable". Use a readable library/provider or the Java tracer in that case.

## Java tracing

Java boot-class hooks depend on ART dispatch and class warmup; install the
tracer after the target has already used crypto, or hook the concrete provider
implementation class instead of the JCA base class.

```sh
./dejavu exec -p <pkg> plugins/algorithm/java/algorithm_java.lua
./dejavu log -p <pkg>          # DejavuHook stream
# stop Java hooks when done:
./dejavu exec -p <pkg> -e "hook.clear()"
```

## Notes

- Output can contain keys, tokens and plaintext; treat logs as sensitive.
- The native tracer covers BoringSSL/libcrypto primitives, not custom
  Flutter/Rust/Java-level algorithms; the Java tracer adds common JCA entry
  points.
- The plugin is unload-safe: `on_unload_prepare` uninstalls its native hooks.
