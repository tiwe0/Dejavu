# Zygisk modularization plan

This document records the original split and the source boundaries in this
checkout. See the [current implementation](#current-implementation).

## Original goal

Reduce high-centrality implementation code without changing the Lua API, Hook
C ABI, LSPlant behavior, control protocol, package layout or supported Android
target.

## Original boundary problem

Before the split, `src/hook_manager.cpp` owned four separate concerns:

1. Hook lifecycle and RCU-style hook table management.
2. Lua management commands.
3. Hook C ABI injection and helper symbol registration.
4. JNI primitive boxing, unboxing and per-call Hook context operations.

The last two concerns were independent of hook installation and accounted for
a large part of the file. The split gave String, object and exception helpers
their own implementation boundaries.

## Initial target modules

- `dejavu_hook_api.h`: stable public Hook C ABI and inline user conveniences.
- `hook_api_runtime.h/.cpp`: private ABI runtime, TinyCC preamble, helper symbol
  registration, JNI primitive conversion and Hook context storage.
- `hook_signature.h/.cpp`: JNI descriptor parsing and normalized value kinds.
- `hook_manager.h/.cpp`: LSPlant installation, record lifetime, Lua control and
  before/original/after orchestration.

String helpers, object type checks and controlled Java calls follow the
reference, buffer, class-loader and exception rules in the
[Hook C ABI](hook-c-api.md#helper-lifetime-contract).

## Behavioral locks

Before and after the move:

- Host target-selection, socket protocol, JNI-signature and Hook API compile
  tests must pass.
- Android API 26 arm64 baseline and API 30/34/35 compatibility targets must
  build with `-Werror` and 16 KB load alignment.
- The MT device protocol test must still cover argument replacement, early
  return, after-result replacement, concurrent dispatch and logical removal.
- The production package must contain the non-stress Agent build.

## Original acceptance criteria

The initial refactor moved Hook C ABI implementation and registration out of
`hook_manager.cpp`. The acceptance run included verification and a return to
the production Agent with no active user hooks.

## Current implementation

- The manager handles Java hook installation, record lifetime and session
  startup. Lua management commands and dispatch have separate implementations.
  [Hook manager](../src/hook_manager.cpp), [hook control](../src/hook_control.cpp)
  and [hook dispatch](../src/hook_dispatch.cpp).
- The ABI runtime defines the TinyCC preamble and registers helper symbols.
  Helpers have their own source files.
  [ABI runtime](../src/hook_api_runtime.cpp), [primitive values](../src/hook_api_primitives.cpp),
  [strings](../src/hook_api_strings.cpp), [Java calls](../src/hook_api_java_call.cpp)
  and [stream events](../src/hook_api_stream.cpp).
- JNI descriptor parsing is separate from hook management. CMake lists the
  Agent and Loader source files. [Hook signatures](../src/hook_signature.cpp)
  and [CMake](../CMakeLists.txt).

The original verification covered host tests, Android arm64 compatibility
builds, the MT Manager protocol stress test and production deployment. That
stress run recorded `calls=160000`, `max_parallel=15`, `java_failures=0` and
ended with an empty production `hook.list()`.
