# Zygisk modularization plan

## Goal

Reduce high-centrality implementation code without changing the Lua API, Hook
C ABI, LSPlant behavior, control protocol, package layout or supported Android
target.

## Current boundary problem

`src/hook_manager.cpp` currently owns four separate concerns:

1. Hook lifecycle and RCU-style hook table management.
2. Lua management commands.
3. Hook C ABI injection and helper symbol registration.
4. JNI primitive boxing, unboxing and per-call Hook context operations.

The last two concerns are independent of hook installation and account for a
large part of the file. Keeping them in the manager makes future String,
object and exception helpers harder to add or review.

## Target modules

- `dejavu_hook_api.h`: stable public Hook C ABI and inline user conveniences.
- `hook_api_runtime.h/.cpp`: private ABI runtime, TinyCC preamble, helper symbol
  registration, JNI primitive conversion and Hook context storage.
- `hook_signature.h/.cpp`: JNI descriptor parsing and normalized value kinds.
- `hook_manager.h/.cpp`: LSPlant installation, record lifetime, Lua control and
  before/original/after orchestration.

The next helper batch is contract-gated. String helpers, object type checks and
controlled Java calls must follow the reference, buffer, class-loader and
exception rules in [hook-c-api.md](hook-c-api.md#helper-lifetime-contract)
before any ABI symbols are added. No helper in that batch may expose `JNIEnv`,
raw `jmethodID`, a borrowed string pointer, C varargs or an implicitly retained
reference.

No new dependency or generalized framework will be introduced. Control and
LSPlant modules remain unchanged.

## Behavioral locks

Before and after the move:

- Host target-selection, socket protocol, JNI-signature and Hook API compile
  tests must pass.
- Android API 35 arm64 must build with `-Werror` and 16 KB load alignment.
- The MT device protocol test must still cover argument replacement, early
  return, after-result replacement, concurrent dispatch and logical removal.
- The production package must contain the non-stress Agent build.

## Stop condition

The refactor is complete when `hook_manager.cpp` no longer implements or
registers Hook C ABI helpers, all verification passes, and the device is
restored to the production Agent with no active user hooks.

## Landed

- `hook_api_runtime.cpp` now owns the TinyCC preamble, JNI primitive boxing and
  unboxing, per-call context helpers, C ABI helper implementations and native
  symbol registration.
- `hook_manager.cpp` now owns hook installation, Lua control registration,
  record lifetime and before/original/after dispatch only.
- Host tests, Android API 35 arm64 stress build, MT Manager protocol stress and
  production deployment all pass. The stress run reached `calls=160000`,
  `max_parallel=15`, `java_failures=0`; the final production `hook.list()` is
  empty.
