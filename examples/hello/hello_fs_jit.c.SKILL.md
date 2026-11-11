---
name: dejavu-plugin-hello-fs-jit
description: TinyCC JIT plugin exercising the host fs interface (read/write/exists/remove) without libc.
---

# hello_fs_jit

Registers:

```lua
fstest.write(path, data) -> bool
fstest.read(path) -> string
fstest.exists(path) -> bool
fstest.remove(path) -> bool
```

Test (use a path in the app files dir):

```sh
./dejavu plugin -p <pkg> install examples/hello/hello_fs_jit.c
./dejavu plugin -p <pkg> call hellofsjit write --arg 's:/data/user_de/0/<pkg>/files/jit-fs.txt' --arg s:hello
./dejavu plugin -p <pkg> call hellofsjit read --arg 's:/data/user_de/0/<pkg>/files/jit-fs.txt'
./dejavu plugin -p <pkg> call hellofsjit exists --arg 's:/data/user_de/0/<pkg>/files/jit-fs.txt'
./dejavu plugin -p <pkg> call hellofsjit remove --arg 's:/data/user_de/0/<pkg>/files/jit-fs.txt'
./dejavu plugin -p <pkg> unload hellofsjit
```

The plugin has `caps=0x8` (FS) and uses only `dejavu_jit_fs_*` host helpers.
