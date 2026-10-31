# Native function hook API

Dejavu can inline-hook exported native functions in an already-loaded module
inside the target process, in addition to Java/Kotlin method hooks. Native
hooks use Dobby (already linked into the agent for LSPlant) and TinyCC for the
replacement function.

The API is available from the persistent Lua control session (CLI, REPL,
`run/<process>.lua`), next to the existing `hook.*` namespace:

```lua
local id = native.install(module, symbol, c_source)  -- returns positive id
local text = native.list()
local hits = native.stats(id)
native.uninstall(id)
local removed = native.clear()
```

## Contract

`native.install(module, symbol, c_source)`:

- `module` must be a shared library already loaded in the target process (for
  example `"libc.so"` or `"libtarget.so"`). It is resolved with
  `dlopen(RTLD_NOW | RTLD_NOLOAD)` and `dlsym`, so the symbol must be
  dynamically exported.
- `symbol` is the exported symbol name (`"read"`, `"getpid"`, a mangled C++
  symbol, ...).
- `c_source` must define exactly one replacement function named
  `dejavu_native_target` with the target function's signature.

The device-side TinyCC preamble provides:

```c
void *dejavu_native_original(void);   /* trampoline to the original function */
void dejavu_native_hit(void);         /* increment this hook's hit counter */
void dejavu_native_log(const char *message);
```

Example: count calls to libc `read` while forwarding to the original:

```lua
local source = [[
long dejavu_native_target(int fd, void *buffer, unsigned long count) {
    dejavu_native_hit();
    return ((long (*)(int, void *, unsigned long))dejavu_native_original())(
        fd, buffer, count);
}
]]
local id = native.install("libc.so", "read", source)
-- ... trigger the target function ...
local hits = native.stats(id)   -- > 0
native.uninstall(id)
```

Typical replacement shapes:

```c
/* replace the return value */
int dejavu_native_target(void) {
    dejavu_native_hit();
    return 1;
}

/* rewrite one argument, forward everything else */
long dejavu_native_target(int fd, void *buffer, unsigned long count) {
    dejavu_native_hit();
    return ((long (*)(int, void *, unsigned long))dejavu_native_original())(
        fd, buffer, count > 4096 ? 4096 : count);
}

/* call the original, then modify its result */
int dejavu_native_target(int value) {
    dejavu_native_hit();
    int result = ((int (*)(int))dejavu_native_original())(value);
    return result + 1;
}
```

## Management

| Call | Meaning |
| --- | --- |
| `native.install(...)` | resolve, compile, and install; returns a positive id |
| `native.stats(id)` | number of replacement calls recorded so far |
| `native.list()` | `id active\|uninstalled module#symbol hits=N` lines |
| `native.uninstall(id)` | restore the original code and release the compiled module |
| `native.clear()` | uninstall all active native hooks; returns the count |

Installed hooks stay active until uninstalled or the process dies. All calls
return normal Lua errors (visible through the CLI / companion log) instead of
crashing the host on validation failures.

## CLI shortcuts

The unified `./dejavu` CLI wraps the common operations (it still executes Lua
in the target process, so the API rules above apply):

```sh
./dejavu native -p com.android.settings list
./dejavu native -p com.android.settings install libc.so read replacement.c
./dejavu native -p com.android.settings stats <id>
./dejavu native -p com.android.settings uninstall <id>
./dejavu native -p com.android.settings clear
```

`install` expects a C file that defines `dejavu_native_target`; the CLI wraps
it into the Lua chunk and prints the returned hook id.

## Limitations (ABI 1)

- **Exported symbols only.** Hidden/static symbols need a resolver/offset API,
  which this ABI version does not provide yet.
- **arm64 Android.** The agent/module only supports `arm64-v8a`.
- **Executable mappings only.** The target address must be in a mapping with
  the execute permission; page protection is saved/widened to patch and then
  restored.
- **Uninstall safety.** Do not uninstall while the target function may be
  executing concurrently: Dobby destroys the trampoline, and an in-flight
  callback that calls `dejavu_native_original()` afterwards is undefined
  behaviour. Uninstalling from an idle period or during a target-process
  restart is safest.
- **Shared state.** Replacements run on arbitrary threads of the target
  process. Keep counters/state atomic (`dejavu_native_hit` is atomic) and do
  not call blocking or allocation-heavy code from hot replacements.
- **Trusted code.** Like Java hooks, native replacements run with the target
  process privileges and cannot be sandboxed by Dejavu. A wrong signature,
  missing trampoline call or ABI mismatch can crash the process.
- **No automatic reload.** After `dejavu deploy-agent` you must restart the
  target app so the new agent (and native API) is loaded.
- Native hooks and LSPlant both use Dobby; hooking the same address that
  LSPlant already manages is not supported. Prefer non-ART functions.

## Validation

The on-device acceptance suite (`./dejavu test -p <process>`) installs a
`libc.so#read` hook, checks that `native.stats()` observes intercepts, calls
the original, and then uninstalls it. See
`zygisk/scripts/device-test.sh`.
