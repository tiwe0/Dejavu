# B1: internal attestation refresh via r0 hook

Status: design/probe complete; not enabled in production.

## Goal

Force X app to run its internal attestation token refresh path instead of only
foregrounding the app and hoping the cache changes.

Entry point:

```text
com.x.attestation.r0#b()V
  -> Attempt attestation
  -> GenerateAttestationTokenV2 / attestation registration
  -> token cache update
```

## Constructor hook result

The real constructor is:

```text
com.x.attestation.r0.<init>(
  kotlinx.coroutines.i0,
  kotlin.coroutines.CoroutineContext,
  com.x.common.api.d,
  com.x.models.gh,
  dev.zacsweers.metro.internal.d,
  com.x.attestation.q,
  com.x.attestation.i,
  com.x.attestation.o,
  com.x.attestation.a,
  com.x.attestation.x,
  com.x.clock.c,
  com.x.app.lifecycle.a,
  com.x.common.api.f,
  com.x.common.api.g)V
```

Installing a runtime hook on this constructor crashed the X app process with
SIGSEGV. The signature was verified with Java reflection, so this was a
constructor-hook/LSPlant issue, not a signature lookup failure.

Do not use constructor hook in production.

## Method hook result

This hook installs and remains active without crashing:

```text
com.x.attestation.r0#b()V
```

It can capture the r0 receiver with:

```c
void *self = dejavu_hook_this_object(context);
```

## Holder

A static AtomicReference exists in the X app:

```java
public abstract class com.x.android.utils.d3 {
    public static final AtomicReference a = new AtomicReference();
}
```

Current code only calls `d3.a.set(...)` from XApplication.onCreate.

The hook can save the captured r0 instance there via controlled Java calls:

```text
Class.forName("com.x.android.utils.d3")
  .getField("a")
  .get(null)
  .set(this_object)
```

The native x_agent plugin can then read `d3.a` through JNI, call `r0.b()`,
and poll the token cache for an expiry increase.

## Loading constraint

The native plugin ABI only installs native inline hooks. It cannot install a
Java method hook.

Java hooks are installed through Lua:

```lua
hook.install("com.x.attestation.r0", "b", "()V", source)
```

DejaVu native plugin autoload currently loads `.so` only. To install the hook
before the app starts / before r0 is used, one of these is needed:

1. a guarded early Lua companion loaded from
   `config/init.lua` when the process is `com.twitter.android`;
2. a framework change that allows early Lua hook plugins;
3. hooking a later method and accepting that the instance is only captured
   after the first natural `r0.b()` call.

Option 1 is the smallest local change.

## Early-init probe result

A guarded early init snippet was tested on one user:

```text
config/init.lua
  -> /proc/self/cmdline contains com.twitter.android
  -> load x_attest_hook.lua
  -> hook.install("com.x.attestation.r0", "b", "()V", source)
```

Result: the app process stayed alive but the DejaVu control target became
disconnected; no X_ATTEST_R0_HOOK_READY log was observed, and hook.list could
not be queried for that PID. The module init.lua was restored from
`init.lua.bak` and the app/signer were restarted.

Conclusion: directly appending a Lua hook to the shared DejaVu init is not
safe enough for production. A proper companion plugin loader/early hook
mechanism is still required.

## Proposed test flow

1. Add guarded early Lua hook for `com.twitter.android`.
2. Hook only `r0#b()`, not the constructor.
3. Capture `this` into `d3.a`.
4. Add native x_agent B1 refresh:
   ```text
   read d3.a
   -> AtomicReference.get
   -> if class is com.x.attestation.r0, invoke b()
   -> poll token cache until expiry increases
   ```
5. Return `refreshed=true` only when expiry actually increases.
6. Test on one user first.

## Risks

- `d3.a` is an app global; verify no other dex reads it before overwriting.
- Constructor hooks are unsafe in current DejaVu/LSPlant.
- Hot unload/reload of the native plugin crashes app processes during
  lifecycle teardown. Update plugins by restarting/waking the app, not by hot
  reload.
- `r0.b()` might not be called if the cached token is still considered valid;
  a natural trigger or a controlled cache state is needed for a first test.
- Restarting an app kills its injected signer; re-inject after app restart.

## Current production state

- 8 X apps running.
- 8 routes online.
- 8 signers pingable.
- No probe hooks remain installed.
- Production config points at `176.122.179.3:39010`.