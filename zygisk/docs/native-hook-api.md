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

## File-based variants

The Lua layer can read a C source file directly from the device instead of
passing the source string through the control channel:

```lua
local id = native.install_file("libc.so", "read", "/data/.../hook.c")
local pid = native.call_file("libc.so", "__getpid", "/data/.../call.c")
```

CLI equivalents:

```sh
./dejavu native -p <pkg> install-file libc.so read /data/.../hook.c
./dejavu native -p <pkg> call-file libc.so __getpid /data/.../call.c
```

The file must be readable by the target process (app files dir is a good
choice), regular, 1 MiB or smaller. The content still follows the same
`dejavu_native_target` / `dejavu_native_call` contracts.

## Management

| Call | Meaning |
| --- | --- |
| `native.install(...)` | resolve, compile, and install; returns a positive id |
| `native.stats(id)` | number of replacement calls recorded so far |
| `native.list()` | `id active\|uninstalled [protection-pending] module#symbol hits=N` lines |
| `native.uninstall(id)` | restore the original entry and saved page permissions |
| `native.clear()` | uninstall all active native hooks; returns the count |

Installed hooks stay active until uninstalled or the process dies. All calls
return normal Lua errors (visible through the CLI / companion log) instead of
crashing the host on validation failures.

## Active calls (native.call)

`native.call(module, symbol, c_source, ...typed_args)` actively invokes an
exported native function through a TinyCC-generated wrapper, without installing
a hook. It returns the wrapper's return value as a Lua integer.

The C source must define `void dejavu_native_call(void)` and may use:

```c
/* injected constant: address of the resolved target function */
DEJAVU_NATIVE_CALL_ADDRESS

/* typed argument accessors; index is 0-based */
long long dejavu_native_call_argument(unsigned int index);          /* int64 view */
long long dejavu_native_call_argument_int(unsigned int index);
double dejavu_native_call_argument_double(unsigned int index);
int dejavu_native_call_argument_boolean(unsigned int index);
const char *dejavu_native_call_argument_string(unsigned int index,
                                               unsigned long *size);
int dejavu_native_call_argument_type(unsigned int index);

/* report the function result (required) */
void dejavu_native_call_return(long long value);
```

## Argument conversion protocol (shared with java.invoke)

Lua values are converted to typed call arguments before the wrapper runs. C
code selects the accessor matching the target signature; using a mismatched
accessor reports an error instead of silently reinterpreting bytes.

| Lua value | Native argument type | Accessor |
| --- | --- | --- |
| `nil` | null | `_argument` returns 0, `_type` returns 0 |
| boolean | boolean | `_argument_boolean` |
| integer | int64 | `_argument_int` / `_argument` |
| number | double | `_argument_double` |
| string | UTF-8 string (pointer + length) | `_argument_string` |
| other | unsupported | error |

`dejavu_native_call_argument_type(i)` returns `0` null, `1` boolean,
`2` integer, `3` number, `4` string, or `-1` on a missing argument. Up to 32
arguments are accepted.

Example: call `getpid()` with no arguments:

```lua
local source = [[
void dejavu_native_call(void) {
    int (*target)(void) = (void *)(unsigned long long)DEJAVU_NATIVE_CALL_ADDRESS;
    dejavu_native_call_return((long long)target());
}
]]
local pid = native.call("libc.so", "getpid", source)   -- e.g. 12345
```

Example with arguments: `abs(-42)`:

```lua
local source = [[
void dejavu_native_call(void) {
    int (*target)(int) = (void *)(unsigned long long)DEJAVU_NATIVE_CALL_ADDRESS;
    int value = (int)dejavu_native_call_argument_int(0);
    dejavu_native_call_return((long long)target(value));
}
]]
local result = native.call("libc.so", "abs", source, -42)   -- 42
```

String and double arguments:

```lua
-- strlen("dejavu") == 6
local strlen_source = [[
void dejavu_native_call(void) {
    unsigned long size = 0;
    const char *text = dejavu_native_call_argument_string(0, &size);
    if (text == 0 || size != 6) { dejavu_native_call_return(-1); return; }
    int (*target)(const char *) = (void *)(unsigned long long)DEJAVU_NATIVE_CALL_ADDRESS;
    dejavu_native_call_return((long long)target(text));
}
]]
local length = native.call("libc.so", "strlen", strlen_source, "dejavu")  -- 6
```

Because the target can be a hooked function, `native.call` is also a reliable
way to trigger a native hook in tests: install the hook, call the function, then
check `native.stats(id)`.

The int64 return value can be passed to a subsequent `native.call` or
`java.invoke` (for example as an integer/long argument). Raw pointers can be
returned and passed back as integers, but that is trusted-code territory: the
wrapper must cast the integer back to the original pointer type.

Limits: up to 32 typed arguments, a single int64 return value, and one compile
per call. The call is synchronous and runs on the control thread, so a
blocking target blocks the Lua control channel until it returns. A mismatched
C signature (as declared in the wrapper) or ABI is undefined behaviour and can
crash the target process; the conversion protocol only validates the Lua-side
types and never reinterprets memory.

## CLI shortcuts

The unified `./dejavu` CLI wraps the common operations (it still executes Lua
in the target process, so the API rules above apply):

```sh
./dejavu native -p com.android.settings list
./dejavu native -p com.android.settings install libc.so read replacement.c
./dejavu native -p com.android.settings call libc.so getpid call.c
./dejavu native -p com.android.settings stats <id>
./dejavu native -p com.android.settings uninstall <id>
./dejavu native -p com.android.settings clear
```

`install` expects a C file that defines `dejavu_native_target`; the CLI wraps
it into the Lua chunk and prints the returned hook id.

## Limitations (ABI 1)

- **Exported symbols first, ELF fallback second.** `dlsym` is tried first; if
  that fails, the loaded image's ELF symbol table is searched (this can reach
  `LOCAL`/non-exported symbols when the image still has `.symtab`). Use
  `module.symbols`/`module.resolve` to discover them; fully stripped images
  without a full symbol table still only expose dynamic symbols.
- **arm64 Android.** The agent/module only supports `arm64-v8a`.
- **Atomic native entry patches.** Native installs require an aligned four-byte
  branch and a fresh anonymous near trampoline. If a suitable near mapping cannot
  be allocated, installation fails before activating the replacement; it does
  not fall back to a multi-instruction entry patch or overwrite code caves.
- **Executable mappings only.** The target address must be in a mapping with
  the execute permission; page protection is saved/widened to patch and then
  restored.
- **In-flight callbacks.** The original trampoline is published before the
  replacement becomes callable. Exposed contexts, JIT code and target-library
  references remain pinned until process exit, so an already-entered callback
  can finish after uninstall. Repeated install/uninstall cycles therefore retain
  memory; restarting the target releases it. Plugin-owned state still needs its
  own synchronization and teardown protocol.
- **Retryable permission errors.** Permission snapshots retain the original
  page addresses and protections across VMA splits. A failed restore is reported
  with the hook id and kept as `protection-pending`; retry `native.uninstall(id)`
  or `native.clear()` rather than assuming the failed operation removed it.
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
