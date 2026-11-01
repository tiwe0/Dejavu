---
name: dejavu-plugin-hello-hook-jit
description: TinyCC JIT plugin that installs a host-tracked Dobby hook and relies on the host to tear it down on unload.
---

# hello_hook_jit

Demonstrates the host hook ownership/teardown contract with a low-traffic
target (`libc.so!atoi`):

1. `hooktest.install` resolves `libc.so!atoi`, calls
   `dejavu_jit_hook_install`, and records no explicit uninstall.
2. On `plugin unload hellohookjit`, the host destroys every hook owned by the
   plugin **before** destroying the JIT module.
3. The process must remain healthy and `atoi` must keep working.

```sh
./dejavu plugin -p <pkg> install plugins/hello/hello_hook_jit.c
./dejavu plugin -p <pkg> call hellohookjit install
./dejavu plugin -p <pkg> call hellohookjit installed
./dejavu plugin -p <pkg> unload hellohookjit
# after unload, call atoi through the agent and verify no crash:
./dejavu native -p <pkg> call-file libc.so atoi /data/.../call_atoi.c --arg s:42
```

The plugin is marked unload-safe; its key point is that it does not manually
remove the hook.
