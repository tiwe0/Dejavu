# Early Lua hooks

The Agent can load one Lua hook script before app code runs, selected by the
target process name:

```text
/data/adb/modules/dejavu_zygisk/config/early-hooks/<process>.lua
```

For X:

```text
config/early-hooks/com.twitter.android.lua
```

The Agent reads the file as root and concatenates it after `config/init.lua`
before starting the persistent Lua control session. The hook script can use the
normal Lua `hook` / `hookx` API; it must not depend on app-accessible file I/O.

This is safer than appending code to the global `config/init.lua`:

- only the selected process loads the hook;
- the script is read by the Agent, not by the app UID;
- no `io.open` / `loadfile` from the app namespace;
- the original `init.lua` stays unchanged.

Example: `plugins/x_agent/hook/x_attest_hook.lua` hooks
`com.x.attestation.r0#b()V` and stores the receiver in
`com.x.android.utils.d3.a`.

Build/deploy the Agent as usual, then install the script under the module
configuration directory. An invalid script must not be added to production
without testing on one target first.
