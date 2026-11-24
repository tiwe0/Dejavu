# algorithm example plugin

Independent crypto/algorithm tracer demonstrating the host `module`, `hook`,
`stream`, and `rpc` services. It does not target a specific app.

- Native `algorithm` plugin hooks libcrypto EVP/HMAC/Cipher/Sign primitives and
  writes algorithm, inputs, outputs, and keys/IVs to a bounded in-memory ring
  buffer (128 events), an optional file, and/or a live TCP/UDP/KCP stream.
- **Router RPC linkage**: registers service `algorithm` with per-process route
  `algorithm.<process>` via the required host `rpc` v1 interface, exposing
  remote `status`, `start`, `stop`, `recent`, `clear`, and `selftest` methods.
- `java/algorithm_java.lua` uses the existing Java hook API to trace
  MessageDigest/Mac/Cipher calls into the `DejavuHook` logcat stream.

## Build & test

```sh
./examples/algorithm/test-host.sh
./examples/algorithm/build.sh
./examples/package.sh
```

## Router example requests

```json
{"type":"request","id":"alg-1","route":"algorithm.com.example.app","method":"start","params":{},"deadline_ms":3000}
{"type":"request","id":"alg-2","route":"algorithm.com.example.app","method":"selftest","params":{},"deadline_ms":3000}
{"type":"request","id":"alg-3","route":"algorithm.com.example.app","method":"recent","params":{"limit":16,"clear":true},"deadline_ms":3000}
{"type":"request","id":"alg-4","route":"algorithm.com.example.app","method":"stop","params":{},"deadline_ms":3000}
```

See [SKILL.md](SKILL.md) for install, start, stream, Router RPC, and Java
tracing commands, and [`router.example.json`](router.example.json) for a
dedicated Router v2 + TLS Hub template.

Treat captured keys/plaintext as sensitive; stop hooks and clear/remove logs
after debugging.
