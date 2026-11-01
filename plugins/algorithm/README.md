# algorithm example plugin

Independent crypto/algorithm tracer demonstrating the host `module`, `hook` and
`stream` services. It does not target a specific app.

- Native `algorithm` plugin hooks libcrypto EVP/HMAC/Cipher/Sign primitives and
  writes algorithm, inputs, outputs, keys/IVs to a file or live stream.
- `java/algorithm_java.lua` uses the existing Java hook API to trace
  MessageDigest/Mac/Cipher calls into the `DejavuHook` logcat stream.

Build:

```sh
./plugins/algorithm/build.sh
```

See [SKILL.md](SKILL.md) for install, start, stream and Java tracing commands.

On Android 15, execute-only APEX libcrypto mappings are rejected by the host
hook layer rather than risking Dobby relocation crashes; the plugin then
reports no hookable symbols. The Java tracer is an example and may need the
concrete provider class for reliable boot-class interception.

Treat captured keys/plaintext as sensitive; stop hooks and remove logs after
debugging.
