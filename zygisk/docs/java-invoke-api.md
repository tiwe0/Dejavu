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
  `android.system.InMemoryDexClassLoader` (API 26+) and returns a ClassLoader
  handle. Use `"<loader_handle>:<class_name>"` as the class argument of
  `java.invoke`/`java.new` to resolve classes from that loader.
- `java.release` deletes the global reference behind a handle. Releasing a
  loader handle does not unload classes already loaded from it.

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
| `Ljava/lang/String;` | string, nil or String handle | UTF-8 ↔ UTF-16 conversion; `nil` is null |
| other object / array descriptor | integer handle or nil | handles come from `java.new` / object return values; no auto array creation |

Return values use the same table in reverse:

| JNI return | Lua value |
| --- | --- |
| `V` | nil |
| `Z` | boolean |
| `B C S I J` | integer |
| `F D` | number |
| `Ljava/lang/String;` | string (UTF-8), or nil |
| other object / array | integer handle, or nil |
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
- Object handles are JNI global references. Up to 1024 live handles are
  allowed; release them with `java.release(handle)` or the process must exit
  to clean them up.
- Arguments and return values are synchronous. A blocking Java call blocks the
  Lua control channel until it returns; there is no timeout or cancellation.

## Limits

- Exact JNI signature required; overloads are selected by signature.
- Arrays are opaque handles only: no element access or array creation yet.
- No field access, no interface dispatch beyond normal JNI `GetMethodID`
  semantics, no Java varargs helpers, no synchronized/wait wrappers.
- Classes are loaded through the target app's ClassLoader (with `FindClass`
  as fallback). Class-loading or resolution failures surface as Lua errors.
- All conversions are range-checked; mismatches are reported as Lua errors.
  Native faults such as a broken ART/JNI state cannot be recovered from.

## Validation

The on-device acceptance suite (`./dejavu test -p <process>`) covers static
conversions (int/long/double/boolean/string), constructor + instance +
release, and error paths (missing method, wrong argument type, missing class).
