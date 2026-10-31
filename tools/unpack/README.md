# Hook-based DEX unpacking

`./dejavu unpack` captures DEX images **when ART registers them**, instead of
scanning memory blindly:

1. The agent installs a `dlopen` / `android_dlopen_ext` hook and logs library
   load events (late `libart.so` loads re-trigger ART hook installation).
2. It installs ART hooks on `ClassLinker::RegisterDexFile` (and
   `RegisterDexFileLocked` when present; `DexFile::OpenCommon` when the ROM
   exposes it) and reconstructs the DEX image from the `DexFile` object.
3. Images mapped to **file-backed** system/app dex are skipped by default
   (they can be extracted offline and would slow startup); only anonymous
   in-memory DEX data is dumped. Set `unpack.include_file_backed(true)` in Lua
   to include them, at the cost of startup latency / ANR risk.

## Usage

```sh
# Persist the output directory in a system property and restart the target so
# the agent enables capture during process startup (before app code runs).
./dejavu unpack -p com.android.settings \
    --out /data/user_de/0/com.android.settings/files/dejavu-unpack

./dejavu unpack -p com.android.settings --status
./dejavu unpack -p com.android.settings --disable   # also clears the property
```

DEX buffers are queued **in memory** when hooks fire and a background writer
flushes them gradually, so the app threads only pay for a snapshot copy. Use
`./dejavu unpack --flush` (or `unpack.flush()`) to wait until everything is
written before pulling. The status shows `pending_files`, `pending_bytes`,
`written_files` and `dropped_budget` (a 128 MiB in-memory budget avoids OOM).

Dumped files land in the device directory together with `manifest`/status
information; pull them as root:

```sh
adb shell su -c 'cp -r /data/user_de/0/com.android.settings/files/dejavu-unpack /sdcard/Download/'
adb pull /sdcard/Download/dejavu-unpack ./dejavu-unpack
```

Advanced Lua control (same process):

```lua
unpack.enable('/data/user_de/0/com.android.settings/files/dejavu-unpack')
unpack.include_file_backed(true)   -- include file-backed dex too
unpack.status()
unpack.dump_memory(address, size, 'label')  -- raw dump for manual captures
unpack.disable()
```

## FART-style extraction (extracting shells)

```lua
unpack.enable(dir)
unpack.hook_invoke()                       -- hook ArtMethod::Invoke
unpack.active_invoke('com.app.A,com.app.B')  -- reflection + null-thread sentinel calls
```

The CLI/helper path uses `unpack.active_invoke_async(names, 40)` so classes
are processed slowly in the background (control session stays responsive);
`active_invoke_wait()`/`status()` track progress. The synchronous
`active_invoke()` remains available for tiny lists.

`active_invoke` resolves each class via the target ClassLoader, enumerates
declared methods with reflection, obtains the ART method pointer with
`FromReflectedMethod`, and calls `ArtMethod::Invoke(art_method, nullptr, ...)`.
The hook detects `thread == nullptr`, records the sentinel hit, zeroes the
result and returns **without executing the method**, so classes with broken
code items do not crash the process. After the calls, discovered DexFile images
are dumped again as `active_revision_*.dex` (code items that were restored in
memory are captured).

Class source from the Manifest main activity namespace (launcher activity class
package), collecting matching classes from dumped dex files:

```sh
./dejavu unpack -p <process> --out <device-dir> --active-invoke-manifest \
    [--class-filter <substring>]
```

From a dumped DEX file:

```sh
./dejavu unpack -p <process> --active-invoke-dex <device-dex-path> \
    --class-filter <optional-name-filter>
```

The agent-side sentinel was validated on Android 15: 104/104 null-thread calls
intercepted, no target method executed, and normal JNI reflection still worked.

## Notes / limits

- Hook installation happens in the Agent's early initialization phase
  (`debug.dejavu.unpack.dir` property), i.e. before the app's own dex files are
  registered. This is what makes early packer loads observable.
- Android 15: `DexFile::OpenCommon` may be absent from the stripped `libart.so`;
  `RegisterDexFile`/`RegisterDexFileLocked` are used instead. ART passes tagged
  pointers (top byte `0xb4`), so the agent clears the top byte before reading
  `DexFile` fields.
- Only supported on arm64, API 26-35, same trust model as hooks: a wrong
  symbol/signature or dumping while the app is highly loaded can crash or ANR
  the target. Use on your own apps / research devices.
- VMP-style or lazy/decrypt-on-demand packers still need class-loading
  traversal and per-load triggers; this tool is the observable DEX-registration
  capture layer, not a universal unpacker.
