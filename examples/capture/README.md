# capture example plugin

`capture` is an unload-safe example Dejavu plugin demonstrating the host
`module`, `hook`, `stream`, and `rpc` services to capture TLS
plaintext at `libssl.so` (`SSL_write` / `SSL_read`).

## Capabilities

- **Lua controls**:
  - `capture.start([path[, transport, endpoint]])` — starts hooks; empty/omitted
    `path` records into the bounded in-memory ring buffer (128 records, 900B
    preview each) and optional TCP/UDP/KCP stream (`1/2/3`);
  - `capture.status()` — call/byte/buffer counters;
  - `capture.stop()` — uninstalls hooks, closes the file and stream;
  - `capture.probe(transport, endpoint)` — sends one test record.
- **Router RPC linkage** (requires host `rpc` v1 interface):
  - Service name: `capture`
  - Per-process route: `capture.<process>` (safe across multiple target apps)
  - Methods:
    - `status` — structured JSON counters, route, PID and active sink state
    - `start` — `{"path"?: "...", "transport"?: 1|2|3, "endpoint"?: "host:port"}` (defaults to memory ring buffer)
    - `stop` — stops capture and returns final counters
    - `recent` — `{"limit"?: 32, "clear"?: false}` returns up to 64 recent captured TLS plaintext records over Router
    - `clear` — empties the in-memory ring buffer
    - `probe` — `{"transport": 1|2|3, "endpoint": "host:port"}`

## Build & test

```sh
./examples/capture/test-host.sh
./examples/capture/build.sh
./examples/package.sh
```

Import `out/plugins/zips/capture.zip` in Manager or install via CLI:

```sh
./dejavu plugin -p <pkg> install out/plugins/capture/libdejavu_capture.so \
    --skill examples/capture/SKILL.md
./dejavu plugin -p <pkg> call capture start
./dejavu plugin -p <pkg> call capture status
./dejavu plugin -p <pkg> call capture stop
./dejavu plugin -p <pkg> unload capture
```

## Router example requests

```json
{"type":"request","id":"cap-1","route":"capture.com.example.app","method":"start","params":{},"deadline_ms":3000}
{"type":"request","id":"cap-2","route":"capture.com.example.app","method":"recent","params":{"limit":16,"clear":true},"deadline_ms":3000}
{"type":"request","id":"cap-3","route":"capture.com.example.app","method":"stop","params":{},"deadline_ms":3000}
```

To bind `capture` to a dedicated Router v2 + TLS Hub, see
[`router.example.json`](router.example.json) and
[`router/plugin-config.md`](../../router/plugin-config.md).

Captured logs and ring-buffer previews can contain sensitive tokens; stop
capture and clear/delete captured output after debugging.
