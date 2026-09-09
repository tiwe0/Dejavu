# Dejavu Hook C ABI v1

The Hook C ABI is injected before every source string passed to `hook.install`.
Hook source must define `before_hook`, `after_hook`, or both:

```c
int before_hook(dejavu_hook_context *context);
int after_hook(dejavu_hook_context *context);
```

Return `DEJAVU_HOOK_OK` on success. A non-zero callback return or any failed
helper marks that phase as failed. Helper failure is sticky, so ignoring a
helper return code does not convert the phase back to success.

## Invocation order

1. Dejavu creates a private copy of the bridge argument array.
2. `before_hook` runs against that copy.
3. Unless before set a result, Dejavu invokes the original method.
4. If the original method did not throw, `after_hook` runs.
5. The final result is returned to LSPlant.

Calling any `dejavu_hook_set_result_*` helper in before skips the original
method. After still runs and may inspect or replace that early result.

If before fails, Dejavu discards argument-array replacements and invokes the
original method with the original argument array. After is skipped. If after
fails, Dejavu returns the result that existed before after began.

These rollbacks cover argument slots and the result reference. They cannot
undo mutations that Hook C performs through an object reference.

## Hook lifetime

`hook.remove(id)` is a fast logical disable. It makes the hook call the
original method without entering `before_hook` or `after_hook`, but leaves the
ART hook and compiled module installed.

`hook.uninstall(id)` is the explicit physical cleanup operation. It disables
the hook, asks LSPlant to restore the target method, waits for callbacks
already in flight to finish, releases the TinyCC module and temporary JNI
global references, and removes the hook from `hook.list()`. The small internal
Record remains as a tombstone because the bridge token is a native pointer;
the backup method reference is retained so an extremely late bridge call can
still reach the original method. Uninstall is not available for the internal
bootstrap hook and fails if LSPlant cannot restore the target.

`hook.clear()` performs logical disable for every user hook. It does not
physically uninstall hooks.

## Arguments and receiver

`dejavu_hook_arg_count(context)` is the number of parameters declared by the
target Java method. Parameter indexes start at zero and never include the
instance receiver.

For an instance method, `dejavu_hook_this_object(context)` returns the receiver.
For a static method it returns null. The receiver cannot be replaced in ABI v1.

Use the channel matching the JNI descriptor:

| JNI descriptors | Getter/setter channel |
| --- | --- |
| `Z B C S I J` | `dejavu_hook_*_arg_i64` |
| `F D` | `dejavu_hook_*_arg_f64` |
| `L...;` and arrays | `dejavu_hook_*_arg_object` |

Integral setters range-check the target descriptor. Object setters do not
check Java assignability in ABI v1; an incompatible object may make the
original invocation throw.

Argument setters are valid only in before. Argument getters are valid in both
phases.

### Typed convenience helpers

Normal Hook C should use the descriptor-specific wrappers instead of manually
converting through `i64` or `f64`:

| Java type | Convenience suffix | C value type |
| --- | --- | --- |
| `boolean` | `boolean` | `int` (`0` or `1`) |
| `byte` | `byte` | `signed char` |
| `char` | `char` | `unsigned short` |
| `short` | `short` | `short` |
| `int` | `int` | `int` |
| `long` | `long` | `long long` |
| `float` | `float` | `float` |
| `double` | `double` | `double` |

For example, an `int` parameter uses `dejavu_hook_get_arg_int` and
`dejavu_hook_set_arg_int`. The low-level `i64` and `f64` functions remain part
of ABI v1 for generic code.

`DEJAVU_HOOK_TRY(expression)` returns immediately from the current callback
when a helper fails. It is intended only inside `before_hook` or `after_hook`,
which both return an integer status.

## Early return and results

The result channel is selected from the method return descriptor:

| JNI return descriptor | Result channel |
| --- | --- |
| `Z B C S I J` | `dejavu_hook_*_result_i64` |
| `F D` | `dejavu_hook_*_result_f64` |
| `L...;` and arrays | `dejavu_hook_*_result_object` |
| `V` | `dejavu_hook_set_result_void` |

Result getters are valid only in after. Result setters are valid in both
phases. Therefore setting a result in before means early return, while setting
one in after means replacing the current return value.

```c
int before_hook(dejavu_hook_context *context) {
    int value = 0;
    DEJAVU_HOOK_TRY(dejavu_hook_get_arg_int(context, 0, &value));
    if (value < 0) {
        return dejavu_hook_return_int(context, 0);
    }
    return dejavu_hook_set_arg_int(context, 0, value + 1);
}

int after_hook(dejavu_hook_context *context) {
    int result = 0;
    DEJAVU_HOOK_TRY(dejavu_hook_get_result_int(context, &result));
    return dejavu_hook_set_result_int(context, result + 1);
}
```

The `dejavu_hook_return_boolean`, `return_byte`, `return_char`, `return_short`,
`return_int`, `return_long`, `return_float`, `return_double`, `return_object`,
`return_null` and `return_void` helpers make early-return intent explicit. They
are aliases over the corresponding result setters, so after may still replace
the value.

## Object references

Object getters and `dejavu_hook_this_object` return JNI local references owned
by the current native dispatch. They are valid only until the callback returns.
Do not store them in global state. Null is a valid object argument or result.

ABI v1 intentionally does not expose `JNIEnv` directly. Stable object
operations should be added as Dejavu helpers instead of embedding a JNI table
layout in generated C.

## Helper lifetime contract

The following rules apply to the String, object-check and controlled Java-call
helpers. They are part of the ABI boundary, not implementation details:

### Callback and JNI references

- A `dejavu_hook_context *` is valid only while the current `before_hook` or
  `after_hook` invocation is running. Helpers are synchronous and must run on
  that same attached Java thread.
- Every object returned through the C ABI is a borrowed JNI local reference.
  This includes argument values, `this`, the current result and object values
  returned by future helpers. It is valid until the callback returns, and the
  generated C code must not retain it, compare it after the callback, or pass it
  to another thread.
- Generated C has no `DeleteLocalRef` operation. The runtime owns temporary
  references created by helpers and releases them at the end of dispatch.
  Argument and result setters copy the reference into the current dispatch
  state; the caller does not release it.
- ABI v1 has no retain/release operation. Any future cross-callback or
  cross-thread reference must use a new explicitly owned handle type and a
  matching release operation; a raw `void *` will never acquire that meaning.

### Strings

String reads will copy bytes into caller-owned storage. They will not return a
pointer into a Java String or into a runtime scratch buffer. The contract is:

- the caller supplies `(buffer, capacity, out_size)`;
- `out_size` reports the byte count excluding the terminating NUL;
- when `capacity` is non-zero, a successful read always writes a terminating
  NUL; insufficient capacity returns `DEJAVU_HOOK_ERROR_RANGE` and still
  reports the required size;
- a null Java reference is not a String and returns
  `DEJAVU_HOOK_ERROR_TYPE`;
- the first version uses JNI Modified UTF-8 semantics, including its encoded
  representation of U+0000. Helpers will use explicit byte lengths so embedded
  modified-UTF-8 bytes are not confused with the C terminator;
- String creation copies all input bytes before the helper returns. The input
  buffer may be stack or temporary storage and need not remain valid afterward.

String setters are valid only in `before_hook`, follow the same descriptor and
null rules as object setters, and do not transfer ownership of the input
buffer. A future standard-UTF-8 API, if needed, will be a separately named
helper rather than silently changing these semantics.

The implemented String symbols are:

- `dejavu_hook_get_arg_string_mutf8`
- `dejavu_hook_set_arg_string_mutf8`
- `dejavu_hook_get_result_string_mutf8`
- `dejavu_hook_set_result_string_mutf8`
- `dejavu_hook_new_string_mutf8`

### Object type checks

Type-check helpers return a boolean/status and never return a class reference.
They resolve classes with the target app's class loader, use JNI internal class
names such as `java/lang/String`, and release all lookup temporaries before
returning. Null is never an instance of a requested type. A failed class lookup
or pending Java exception is converted to `DEJAVU_HOOK_ERROR_JNI` and cleared.
The initial helper only answers `instanceof`; assignability and array-specific
queries require separate contracts. The implemented symbol is
`dejavu_hook_is_instance_of`.

### Controlled Java method calls

The call helper will be descriptor-driven and synchronous:

- class name, method name and the complete JNI descriptor are supplied
  explicitly; no reflective `Method` or raw `jmethodID` is exposed to C;
- the receiver is either a borrowed object reference or null for a static call;
- arguments use a fixed tagged value structure, not C varargs. This keeps the
  ABI deterministic on arm64 and lets the runtime validate primitive/object
  kinds before entering Java;
- primitive results are written to caller-owned output storage. Object and
  String results follow the borrowed-reference rule above;
- class resolution uses the target app class loader associated with the hook;
  lookup and invocation temporaries are released before the helper returns;
- Java exceptions and JNI failures are cleared and returned as
  `DEJAVU_HOOK_ERROR_JNI`; a failed helper never leaves an accidental pending
  exception for the original method;
- calls are not asynchronous, must not outlive the callback, and must not be
  used to implement a background worker. Recursive Hook dispatch on the same
  thread follows the existing re-entry bypass rule.

These rules intentionally leave no borrowed pointer or JNI reference usable
after a callback. They also make helper failure compatible with the existing
sticky `helper_status` and fail-open dispatch behavior.

The implemented Java call symbol is `dejavu_hook_call`. Its `flags` currently
accept `DEJAVU_HOOK_CALL_STATIC` or zero for an instance call. Arguments and
results use `dejavu_hook_value`: integral descriptors use `I64`, `Z` may use
`BOOLEAN`, floating descriptors use `F64`, and object descriptors use
`OBJECT` or `STRING` (the latter is copied with Modified UTF-8 semantics).
Void calls return `VALUE_VOID`.

## Exceptions and concurrency

If the original Java method throws, Dejavu skips after and does not clear the
exception produced by the original invocation. The current LSPlant backup
path invokes `java.lang.reflect.Method.invoke`, so Java callers observe the
standard reflective exception wrapper and its original cause. Helper-generated
JNI exceptions are cleared and reported as `DEJAVU_HOOK_ERROR_JNI` so the phase
can follow its fail-open rule.

Dejavu cannot recover from native faults such as `SIGSEGV`, stack corruption,
an invalid function pointer, or a wrong external symbol declaration. Such a
fault can terminate the target process. ABI v1 does not install a signal-based
recovery path, and it does not promise a timeout for a callback: a callback
that loops or blocks cannot be safely killed from the control channel.

These limits are intentional. ABI v1 does not add exception objects, owned
cross-thread references, worker-process isolation, or a new native fault
protocol. Native hook code is trusted to obey the callback signature and the
reference-lifetime rules above.

Callbacks can run concurrently on multiple Java threads. Hook C must keep
shared mutable state atomic or otherwise synchronized. Calls made recursively
on the same thread while a Dejavu callback is active bypass Dejavu callbacks
and invoke their originals.

## Stable metadata

- `dejavu_hook_abi_version()` returns `DEJAVU_HOOK_ABI_VERSION` (`1`).
- `dejavu_hook_phase(context)` returns BEFORE or AFTER.
- `dejavu_hook_id(context)` returns the ID returned by `hook.install`.
- `dejavu_hook_log(message)` writes to the `DejavuHook` logcat tag.

Adding helpers is backward compatible within ABI v1. Changing callback
signatures, index semantics, result semantics, or reference lifetime requires
a new ABI version.
