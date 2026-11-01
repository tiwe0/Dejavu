---
name: dejavu-plugin-hello
description: Contract/test plugin for the Dejavu native plugin ABI. Use it to verify plugin load, host mem/module/hook services, plugin.call and safe unload.
---

# hello plugin

Minimal **unload-safe** Dejavu plugin. It is a contract test for the plugin ABI,
not a production hook.

## Functions

- `hello.greet` — returns `hello from the Dejavu plugin ABI`.
- `hello.info` — returns version, available host interfaces and checks
  `mem.query` + `module.resolve("libc.so", "getpid")`.

## Build / deploy

```sh
./plugins/hello/build.sh
adb -s "$ADB_SERIAL" push out/plugins/hello/libdejavu_hello.so /data/local/tmp/
adb -s "$ADB_SERIAL" shell su -c '
  cp /data/local/tmp/libdejavu_hello.so \
     /data/user_de/0/<pkg>/files/libdejavu_hello.so
  cp /data/local/tmp/libdejavu_hello.so.SKILL.md \
     /data/user_de/0/<pkg>/files/libdejavu_hello.so.SKILL.md 2>/dev/null || true
  chmod 755 /data/user_de/0/<pkg>/files/libdejavu_hello.so
  chcon u:object_r:apk_data_file:s0 /data/user_de/0/<pkg>/files/libdejavu_hello.so
'
```

Load and exercise it:

```sh
./dejavu plugin -p <pkg> allow /data/user_de/0/<pkg>/files/libdejavu_hello.so
./dejavu plugin -p <pkg> load  /data/user_de/0/<pkg>/files/libdejavu_hello.so
./dejavu plugin -p <pkg> call hello greet
./dejavu plugin -p <pkg> call hello info
./dejavu plugin -p <pkg> unload hello
```

Expected `hello.info` includes `interfaces=mem,module,hook query=ok resolve=ok`.

## Agent notes

- Use this plugin first when validating a new device/ROM: if load/call/unload
  works, the ABI and host services are healthy.
- It is unload-safe; any crash or error during `unload` indicates a host ABI or
  registration bug, not a hook-teardown issue.
- Do not use it as a production hook; it has no native hook installations.
