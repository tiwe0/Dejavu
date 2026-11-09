---
name: dejavu-plugin-hello-jit
description: TinyCC JIT plugin example for Dejavu. Load the C source directly through plugin.load_jit and call hellojit.greet/info without NDK compilation.
---

# hello_jit

This is the **JIT version** of the hello contract plugin. The agent reads the
`.c` source from the device, prepends the TinyCC JIT ABI prelude, compiles it in
memory with TinyCC and registers `hellojit.greet` / `hellojit.info`.

## Deploy / load

```sh
adb -s "$ADB_SERIAL" push examples/hello/hello_jit.c /data/local/tmp/
adb -s "$ADB_SERIAL" shell su -c '
  cp /data/local/tmp/hello_jit.c \
     /data/user_de/0/<pkg>/files/hello_jit.c
  cp .../hello_jit.c.SKILL.md \
     /data/user_de/0/<pkg>/files/hello_jit.c.SKILL.md
  chmod 644 /data/user_de/0/<pkg>/files/hello_jit.c
'
./dejavu plugin -p <pkg> allow /data/user_de/0/<pkg>/files/hello_jit.c
./dejavu plugin -p <pkg> jit /data/user_de/0/<pkg>/files/hello_jit.c
./dejavu plugin -p <pkg> call hellojit greet
./dejavu plugin -p <pkg> call hellojit add --arg 20 --arg 22
./dejavu plugin -p <pkg> call hellojit info
./dejavu plugin -p <pkg> unload hellojit
```

Expected `hellojit.info` returns `JIT plugin: mem=ok module=ok`.

## Notes

- The source is C only (TinyCC), no C++ features, no headers/libc; host
  functions such as `dejavu_jit_alloc`, `dejavu_jit_mem_*`,
  `dejavu_jit_module_resolve`, `dejavu_jit_hook_*` are provided by the prelude.
- JIT plugins require the Lua control engine; load them after startup.
- `plugin.load_jit()` reads the file, prepends the generated prelude and calls
  `dejavu_engine_compile()` internally. `dejavu_engine_compile_file()` is a
  separate public convenience API used by host tooling, not by this loader.
