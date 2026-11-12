---
name: dejavu-plugin-rpc-echo
description: Minimal native business RPC example for verifying the Dejavu host rpc interface, Unix registration, callback results/errors, cancellation and safe unload without account data.
---

# RPC Echo

Read [the business RPC contract](../../zygisk/docs/plugin-rpc.md) when implementing
another plugin. This example declares route `example.echo` and methods `echo`,
`status` and `fail`; the framework owns all Unix socket transport and request state.

Build with `examples/rpc_echo/build.sh`, run the callback contract with
`examples/rpc_echo/test-host.sh`, then load into one authorized test process.
Use a synthetic `echo` request or `status`; no account lookup or external requests
are performed. The example persists no RPC body or history.
