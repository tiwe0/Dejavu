---
name: dejavu-plugin-hello-hook-jit
description: TinyCC JIT example of removing a host-tracked hook during unload preparation.
---

# hello_hook_jit

This unload-safe example exercises the host's hook ownership and teardown
contract with `libc.so!atoi`.

1. `hooktest.install` installs the hook through `dejavu_jit_hook_install` and
   saves its target for cleanup.
2. `dejavu_jit_plugin_unload_prepare` calls `dejavu_jit_hook_uninstall` for its
   own target. The host then attempts to remove any remaining tracked hooks.
   If cleanup fails, the module stays loaded so unload can be retried.
3. On success, the host calls `dejavu_jit_plugin_shutdown` and releases its JIT
   module reference. A cached module may remain resident.

```sh
./dejavu plugin -p <pkg> install examples/hello/hello_hook_jit.c
./dejavu plugin -p <pkg> call hellohookjit install
./dejavu plugin -p <pkg> call hellohookjit installed
./dejavu plugin -p <pkg> unload hellohookjit
# after unload, call atoi through the agent and verify no crash:
./dejavu native -p <pkg> call-file libc.so atoi /data/.../call_atoi.c --arg s:42
```

[Plugin unload contract](../../zygisk/docs/plugin-api.md#unloading)
