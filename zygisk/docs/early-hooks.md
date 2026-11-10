# Per-process startup Lua hooks

The Agent can load an optional startup script for an exact target process:

```text
/data/adb/modules/dejavu_zygisk/config/early-hooks/com.example.app.lua
/data/adb/modules/dejavu_zygisk/config/early-hooks/com.example.app:worker.lua
```

The main process and a `:worker` process select independent files. Installing a
script does not add the process to `config/targets.txt` or enable injection.
Scripts are read through the module directory descriptor during privileged
Agent initialization; execution uses the retained source, without app-side
`io.open` or `loadfile` access to root-owned module files. Keep these files and
their parent directories root-owned and private.

## Timing and execution

The existing bootstrap invokes the original
`ActivityThread.handleBindApplication` first, then resolves the application's
ClassLoader. At that point the Agent initializes its persistent Lua session with
`config/init.lua`, evaluates the process-specific startup script as a **separate
chunk in the same session**, and finally starts live RPC service.

Despite the directory name, this is **not** a pre-application interception
point: application initialization and `Application.onCreate` may already have
run. It cannot reliably intercept those earlier calls or class initialization.
The script may use the normal `hook` / `hookx` APIs against the available app
ClassLoader. Globals are shared with the persistent session; chunk-local
variables are not. A top-level `return` in `init.lua` does not skip or corrupt
the separate startup script. The global `init.lua` is never modified.

## Errors and recovery

- A missing process script is optional and leaves normal startup unchanged.
- Files must be regular text files of at most 1 MiB, without NUL bytes. Leaf
  symlinks, unreadable files and invalid configured paths are logged as errors;
  the Agent continues without that optional script.
- An `init.lua` failure prevents control initialization, as before.
- Startup-script syntax or runtime errors are logged, while an initialized
  session still starts live RPC for inspection and recovery. A runtime error
  does not automatically roll back hooks or other changes already made.

Installing or changing a script affects the next start of that target process.
Test a script on a controlled process before using it for other targets.
