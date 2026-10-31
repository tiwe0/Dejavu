# Maintained patches

This directory contains patches that are applied at build time. The aim is to
keep every pinned dependency pinned while making its build behavior explicit
and testable.

## `tinycc-runtime-safety.patch`

Applied to the pristine files exported from the pinned `extern/tinycc`
submodule (`git archive HEAD`) by:

- `scripts/smoke-host.sh`
- `scripts/build-android-arm64.sh`

The patched sources are compiled from a temporary copy under the build output
directory, so **the submodule itself is never modified** and its working tree
stays clean.

### What it changes

1. **Writable-data page separation.** `tcc_relocate_ex()` lays executable
   sections directly before non-executable sections (`.data`, `.bss`). The
   following `set_pages_executable()` call mprotects the code pages as
   `PROT_READ | PROT_EXEC`; with small code sizes the data page shares that
   range and becomes read-only, so generated code that writes a static/global
   crashes with `SIGSEGV`. The patch page-aligns the first non-executable
   section using the runtime page size, so `mprotect()` can never cover
   writable data.
2. **Non-fatal relocation errors.** TCC's default error path calls `exit(1)`
   for fatal errors raised outside the compile-time jump buffer (for example
   `mprotect()` denial on execmem-restricted ROMs). The patch wraps
   `tcc_relocate()` in a `setjmp` handler, routes these errors through the
   configured error callback, releases the TCC state semaphore for both the
   `longjmp` and `tcc_error_noabort` paths, and returns `-1` so embedders can
   report an error instead of killing the host process.

### Upstream status

The patch targets the **Frida fork** of TinyCC pinned by this repository
(`ca5ee011`, branded `0.9.27-frida`). It has not been submitted upstream; the
frida/tinycc fork and the mobile TinyCC upstream evolve separately. When
bumping the submodule, check whether either fix is available upstream and
whether the patch still applies.

### Upgrade procedure

1. Update the `extern/tinycc` gitlink and re-run
   `git submodule update --init --recursive`.
2. Regenerate the patch: export the new pristine tree, apply the intended
   edits, and produce a unified diff with `a/tccrun.c` / `b/tccrun.c` labels.
3. Verify it applies cleanly:
   `git -C extern/tinycc archive HEAD | tar -x -C /tmp/tcc && patch -d /tmp/tcc -p1 --dry-run < patches/tinycc-runtime-safety.patch`
4. Run `make host-smoke` (includes the writable-global regression and, on
   Linux, the mprotect-failure regression), all Android API builds and the
   on-device acceptance script:
   `TARGET_PROCESS=<package> ./zygisk/scripts/device-test.sh`.
5. Update this document and `THIRD_PARTY_NOTICES.md` if the pin/licensing
   changes.

### Removal criteria

The patch can be dropped when the pinned TinyCC revision (or its replacement)
places non-executable sections on separate pages before protecting executable
pages, and when `tcc_relocate()` returns an error instead of exiting on
relocation failures.

## Other patches

- `zygisk/patches/lsplant-main-reflection-shorty.patch` — applied by
  `zygisk/scripts/fetch-third-party.sh`; see `zygisk/third_party/README.md` for
  the rationale and pinned LSPlant revision.
