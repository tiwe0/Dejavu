# Java active invocation API

Besides hooking, the Agent can actively call Java methods from the persistent
Lua control session (CLI, REPL, `run/<process>.lua`):

```lua
local text = java.invoke(class_name, method_name, jni_signature, ...args)
local text = java.invoke(handle, method_name, jni_signature, ...args)
local handle = java.new(class_name, constructor_signature, ...args)
local loader = java.load_dex(device_dex_path)
java.release(handle)
```

- `java.invoke` is a **static** method call when the first argument is a class
  name (string), and an **instance** method call when it is an object handle
  (integer).
- `java.new` constructs an object and returns a process-session scoped integer
  handle.
- `java.load_dex(path)` loads a raw `.dex` file with
  `dalvik.system.InMemoryDexClassLoader` (API 26+) and returns a ClassLoader
  handle. Use `"<loader_handle>:<class_name>"` (or `java.use(cls, loader)`) to
  resolve classes from that loader.
- `java.release` deletes the global reference behind a handle. Releasing a
  loader handle does not unload classes already loaded from it.

## Frida-style proxy API (`java.use`, `obj:method(...)`, fields & arrays)

In addition to raw JNI signatures, the persistent Lua session provides a cached
reflection proxy layer built on top of `java.invoke`:

```lua
local app <close> = java.app() -- shortcut for ActivityThread.currentApplication()
local pkg = app:getPackageName()
local intent <close> = app:getPackageManager():getLaunchIntentForPackage(pkg)

-- Direct class call construction + proxy arguments (instance & Class<?>)
local Intent = java.use("android.content.Intent")
local MainActivity = java.use("com.example.MainActivity")
local explicit <close> = Intent(app, MainActivity)

-- Overload auto-matching, explicit :overload(...), and typed primitive / null markers
local StringBuilder = java.use("java.lang.StringBuilder")
local sb <close> = StringBuilder("hello")
sb:append(" world")
local String = java.use("java.lang.String")
local t1 = String.valueOf:overload("long")(42)
local t2 = String.valueOf(java.long(42))
obj:ambig(java.null("java.io.File"))

-- Static/instance fields, __cast views, __instanceof, __str, and REPL __inspect
local Build = java.use("android.os.Build")
local model = Build.MODEL
obj.timeout = 5000
local base = obj:__cast("com.example.Base") -- also accepts a class proxy
if obj:__instanceof(base:__class()) then print(obj:__str()) end
print(StringBuilder:__inspect("append"))

-- Arrays (1-based Lua arr[i]/ipairs + 0-based __get/__set) and binary-safe byte[]
local MD = java.use("java.security.MessageDigest")
local md <close> = MD.getInstance("SHA-256")
local digest <close> = md:digest(java.bytes("hello"))
local raw_bytes = digest:__bytes()
local names <close> = java.array("java.lang.String", {"a", "b"})
for i, v in ipairs(names) do print(i, v) end

-- Auto-closing collection/Map iteration & recursive Java -> Lua conversion (ideal for RPC!)
-- Arrays/Iterables yield (1-based index, item) so null elements never truncate the loop.
for i, item in java.each(pkgList) do print(i, item and item.packageName) end
-- Maps yield (key_or_java_NULL, value) so null keys never truncate the loop.
for k, v in java.each(headersMap) do print(k == java.NULL and "null" or k:__str(), v and v:__str()) end
local plain_lua = java.to_lua(resultMap, { bytes = "hex" }) -- JSON/UTF-8 safe

-- Direct hook installation from a static or instance method proxy (or :overload)
local hook_id = String.valueOf:overload("long"):hook(c_source)
local on_create_id = java.use("android.app.Activity").onCreate:hook(c_source)
```

To avoid collisions with real Java method names (such as `WakeLock:release()` or
`obj:id()`), proxy meta-operations use the `__` prefix (`__id`, `__is_alive`,
`__release`, `__detach`, `__class`, `__class_name`, `__instanceof`, `__str`,
`__inspect`, `__each`, `__cast`, `__invoke`, `__field`, `__set_field`,
`__length`, `__get`, `__set`, `__bytes`, `__to_table`, `Cls:__new`) alongside
module helpers (`java.app`, `java.release`, `java.id`, `java.detach`,
`java.is_alive`, `java.is_ref`, `java.is_class`, `java.cast`, `java.instanceof`,
`java.str`, `java.each`, `java.to_lua`, `java.bytes`, `java.array`, `java.NULL`,
`java.boolean`/`byte`/`char`/`short`/`int`/`long`/`float`/`double`/`null`).

## Automatic handle release & resource scopes

Inside the persistent Lua control session, handles returned by `java.new`,
`java.load_dex`, and object/array-returning `java.invoke` calls are wrapped in
managed Lua 5.4 references:

- **GC fallback (`__gc`)**: dropping the last Lua reference releases the
  underlying JNI global reference automatically on garbage collection.
- **Deterministic `<close>` (`__close`)**: use `local obj <close> = java.new(...)`
  to release immediately when leaving a lexical block (even on error).
- **Scoped cleanup (`java.scope`)**: wraps a function and releases every Java
  handle created inside it in LIFO order on return or error; call `scope:keep(h)`
  to promote a handle out of the scope, or `h:__detach()` / `java.detach(h)` to
  transfer raw integer ownership (disabling GC auto-release).
- **Explicit `java.release(h)` / `h:__release()`**: remains supported and disarms
  `__gc` so double-release cannot occur. `loader .. ":com.example.Cls"` works
  through `__concat`.
- **Registration resources (`dejavu.scope()` / Manager `ctx.track_*`)**: long-lived
  registrations (`rpc.register`, `hook.install`, `native.install*`,
  `hook.stream_acquire`) are **not** tied to `__gc`; track them with
  `dejavu.scope()` or Manager plugin `ctx.track_rpc` / `ctx.track_hook` /
  `ctx.track_native_hook` / `ctx.track_stream` / `ctx.track_java` / `ctx.defer`
  for deterministic LIFO cleanup (with retryable `"busy"` preservation on `stop`).

## Examples

Static methods and primitive conversions:

```lua
local text = java.invoke("java.lang.String", "valueOf", "(I)Ljava/lang/String;", 42)
-- text == "42"

local value = java.invoke("java.lang.Integer", "parseInt", "(Ljava/lang/String;)I", "123")
-- value == 123

local big = java.invoke("java.lang.Long", "parseLong", "(Ljava/lang/String;)J",
                        "9007199254740993")
local decimal = java.invoke("java.lang.Double", "parseDouble", "(Ljava/lang/String;)D", 2.5)
local flag = java.invoke("java.lang.Boolean", "parseBoolean", "(Ljava/lang/String;)Z", "true")
```

Dex class loader:

```lua
local loader = java.load_dex("/data/user_de/0/com.android.settings/files/probe.dex")
local text = java.invoke(loader .. ":dexprobe.Probe", "hello",
                         "(Ljava/lang/String;)Ljava/lang/String;", "world")
local value = java.invoke(loader .. ":dexprobe.Probe", "add", "(II)I", 3, 4)
local object = java.new(loader .. ":dexprobe.Probe", "(Ljava/lang/String;)V", "abc")
local value_text = java.invoke(object, "value", "()Ljava/lang/String;")
java.release(object)
java.release(loader)
```

`java.load_dex` accepts a raw dex file readable by the target process; it does
not extract `classes.dex` from APK/JAR files. The dex is loaded in memory, so
no writable optimized-directory is needed.

Constructors and instance methods:

```lua
local handle = java.new("java.lang.StringBuilder", "(Ljava/lang/String;)V", "abc")
local text = java.invoke(handle, "toString", "()Ljava/lang/String;")  -- "abc"
java.release(handle)
```

The CLI exposes the same calls:

```sh
./dejavu java -p com.android.settings invoke java.lang.String valueOf \
    '(I)Ljava/lang/String;' --arg 42
./dejavu java -p com.android.settings new java.lang.StringBuilder \
    '(Ljava/lang/String;)V' --arg abc
./dejavu java -p com.android.settings invoke 1 toString '()Ljava/lang/String;'
./dejavu java -p com.android.settings release 1
./dejavu java -p com.android.settings load-dex \
    /data/user_de/0/com.android.settings/files/probe.dex
./dejavu java -p com.android.settings invoke 2:dexprobe.Probe add '(II)I' --arg 3 --arg 4
```

## Argument conversion protocol

The JNI signature is authoritative; every Lua argument must match the
corresponding parameter descriptor.

| JNI descriptor | Lua value | Notes |
| --- | --- | --- |
| `Z` | boolean | `true`/`false` only |
| `B` | integer | -128..127 |
| `C` | integer | 0..65535 |
| `S` | integer | -32768..32767 |
| `I` | integer | 32-bit signed range |
| `J` | integer | 64-bit signed; Lua 5.4 integers are exact |
| `F` | number or integer | converted to `float` |
| `D` | number or integer | converted to `double` |
| `Ljava/lang/String;` / `Ljava/lang/CharSequence;` / `Ljava/lang/Object;` | string, nil, or object/class proxy | UTF-8 ↔ UTF-16 conversion for Lua strings; `nil` is null |
| other object / array descriptor | object proxy, class proxy (`Class<?>`), integer handle, or nil | validated with JNI `IsInstanceOf`; create arrays with `java.array` / `java.bytes` |

Return values use the same table in reverse:

| JNI return | Lua value |
| --- | --- |
| `V` | nil |
| `Z` | boolean |
| `B C S I J` | integer |
| `F D` | number |
| `Ljava/lang/String;` | string (UTF-8), or nil |
| other object / array | managed Java proxy handle (or raw integer via CLI), or nil |
| Java exception | Lua error (`Throwable.toString()`, exception cleared) |

## Chaining invocation results

Every return value can be fed into the next invocation:

- Primitive results and strings are ordinary Lua values, so they can be passed
  directly to another `java.invoke`.
- Object and array results are integer handles. They can be used as an instance
  receiver or as a reference argument, then released with `java.release`.
- Calls can be nested because handles are created before the outer invocation
  runs.

```lua
-- nested: java.new(...) returns a handle that java.invoke uses immediately
local length = java.invoke(
    java.new("java.lang.StringBuilder", "(Ljava/lang/String;)V", "abcd"),
    "length", "()I")                                  -- 4

-- returned String -> next call argument
local builder = java.new("java.lang.StringBuilder", "(Ljava/lang/String;)V", "123")
local text = java.invoke(builder, "toString", "()Ljava/lang/String;")
java.release(builder)
local parsed = java.invoke("java.lang.Integer", "parseInt", "(Ljava/lang/String;)I", text)
-- parsed == 123

-- returned array handle -> another call argument
local string_handle = java.new("java.lang.String", "(Ljava/lang/String;)V", "a,b,c")
local parts = java.invoke(string_handle, "split", "(Ljava/lang/String;)[Ljava/lang/String;", ",")
local count = java.invoke("java.lang.reflect.Array", "getLength",
                          "(Ljava/lang/Object;)I", parts)
java.release(parts)
-- count == 3
```

Lifetime rules still apply: a handle is valid until `java.release` or target
process exit. Passing a released handle fails with a clear Lua error. When
nesting object-returning calls, release each intermediate handle unless it is
the receiver you want to keep.

Cross-API chaining works for scalar values: an integer returned by
`native.call` can be passed to `java.invoke` (for example as a `J` argument),
and a primitive returned by `java.invoke` can be passed to `native.call`.
Object handles are *not* jobject pointers and only have meaning to
`java.invoke`/`java.new`/`java.release`.

## Handles and lifetimes

- Handles are positive integers scoped to the target process. They stay valid
  across CLI reconnects and REPL commands while the process lives; restarting
  the target app invalidates them.
- Object handles are JNI global references. Up to 8192 live handles are
  allowed; release them with `<close>`, `java.scope`, `java.release(handle)`,
  or drop references for `__gc` cleanup.
- Arguments and return values are synchronous. A blocking Java call blocks the
  Lua control channel until it returns; there is no timeout or cancellation.

## Interactive CLI REPL

Launch an interactive Lua session in a running process:

```bash
./dejavu repl -p com.example.app
```

Features:

- **Expression auto-return & pretty-printing**: expressions evaluate without
  writing `return`, `print(...)` output is captured inline, `_` holds the last
  non-nil result, and Java proxies/arrays/tables are formatted cleanly
  (including hex previews for short `byte[]` arrays).
- **Multiline input & history**: incomplete Lua chunks continue on `... > `
  (press `Ctrl-C` to cancel a block without exiting), and history persists in
  `~/.dejavu_repl_history` (override via `--history` or `DEJAVU_REPL_HISTORY`).
- **Live Tab completion**: completes globals, Lua tables, `Cls:method` /
  `Cls.field` / `Cls.method:overload`, `obj:method`, and dot-commands without
  invoking getters or user metamethods.
- **Dot-commands**: `.help`, `.use <class> [alias]`, `.inspect <expr> [filter]`,
  `.load <file.lua>`, `.hooks`, `.gc`, `.reconnect`, `.raw <lua>`, `.clear`,
  `.exit` / `.quit`.

## Limits

- `method:hook(c_source)` delegates to LSPlant (`hook.install`), which only
  hooks methods (not `<init>` constructors) in the target app's ClassLoader
  (not classes loaded via `java.load_dex`).
- No automatic Java varargs packing (`Object...` expects a `java.array(...)`),
  and no `synchronized` / `Object.wait` wrappers.
- Classes are loaded through the target app's ClassLoader (with `FindClass`
  as fallback). Class-loading or resolution failures surface as Lua errors.
- All conversions, casts, field lookups, and array operations are type- and
  range-checked; mismatches are reported as Lua errors. Native faults such as
  a broken ART/JNI state cannot be recovered from.

## Validation

The on-device acceptance suite (`./dejavu test -p <process>`) covers static
conversions (int/long/double/boolean/string), constructor + instance +
release, and error paths (missing method, wrong argument type, missing class).
