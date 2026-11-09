---
name: dejavu-plugin-hello-hook-jit
description: TinyCC JIT plugin that installs a host-tracked Dobby hook and relies on the host to tear it down on unload.
---

# hello_hook_jit

Demonstrates the host hook ownership/teardown contract with a low-traffic
target (`libc.so!atoi`):

1. `hooktest.install` resolves `libc.so!atoi`, calls
   `dejavu_jit_hook_install`, and records no explicit uninstall.
2. `dejavu_jit_plugin_unload_prepare` calls `dejavu_jit_hook_uninstall` for its
   own target. The host rejects `plugin.unload` while any host-tracked hook is
   still owned by the plugin, so teardown cannot race Dobby callbacks.
3. After unload the host destroys the JIT module; the process must remain
   healthy and `atoi` must keep working.

```sh
./dejavu plugin -p <pkg> install examples/hello/hello_hook_jit.c
./dejavu plugin -p <pkg> call hellohookjit install
./dejavu plugin -p <pkg> call hellohookjit installed
./dejavu plugin -p <pkg> unload hellohookjit
# after unload, call atoi through the agent and verify no crash:
./dejavu native -p <pkg> call-file libc.so atoi /data/.../call_atoi.c --arg s:42
```

The plugin is marked unload-safe; its key point is that it removes its own
hook in shutdown and the host enforces this before module teardown.
