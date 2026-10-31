# Agent helper APIs

The persistent Lua session also exposes filesystem and shared-library helpers.
They run **inside the target process** and are not a sandbox: file access uses
the target app's UID/SELinux domain, and libraries are loaded into that
process. Only use them on devices and processes you control.

## Filesystem (`fs.*`)

```lua
local text  = fs.read(path)              -- string (binary safe)
local bytes = fs.write(path, data[, append])  -- integer bytes written
local exists = fs.exists(path)           -- boolean
fs.remove(path)                          -- true, or a Lua error
local names = fs.list(directory)         -- newline-separated entries
local info  = fs.stat(path)              -- "size=... mode=... mtime=... type=..."
```

- `fs.write` creates files with mode `0644`; pass `true` as the third argument
  to append instead of truncating.
- `fs.read` refuses non-regular files and files larger than 64 MiB.
- Paths are resolved by the target process, so app-private directories work
  only when the target app can access them. A common pattern is to ask the app
  for its own directory first:

```lua
local app = java.invoke('android.app.ActivityThread', 'currentApplication',
    '()Landroid/app/Application;')
local files = java.invoke(app, 'getFilesDir', '()Ljava/io/File;')
local dir = java.invoke(files, 'getAbsolutePath', '()Ljava/lang/String;')
java.release(files)
java.release(app)

local path = dir .. '/note.txt'
fs.write(path, 'hello')
local content = fs.read(path)          -- "hello"
fs.remove(path)
```

## Shared libraries and symbols (`module.*`)

```lua
local id = module.load(path[, 'local'|'global'])  -- dlopen(RTLD_NOW)
module.unload(id)                                 -- true
local text = module.loaded()                      -- "id<TAB>path" lines
local symbols = module.symbols(target[, filter])  -- symbol table lines
local address = module.resolve(target, symbol)    -- integer address or nil
```

- `target` is either a library id returned by `module.load`, or a library
  name/path that is already loaded (for example `libc.so`).
- `module.load` uses `RTLD_NOW`; the default scope is `local`, pass `global` for
  `RTLD_GLOBAL`.
- `module.symbols` reads the **on-disk ELF image of the loaded library**
  (`.dynsym` + `.symtab`) and returns lines:

```text
name<TAB>TYPE<TAB>BIND<TAB>0xaddress
```

  `TYPE` is `F` (function), `O` (object), `T` (TLS), `I` (ifunc), `?` (other);
  `BIND` is `GLOBAL`, `LOCAL` or `WEAK`. Duplicate names from the dynamic and
  full tables are collapsed (the dynamic entry wins). A substring filter can be
  passed as the second argument, e.g. `module.symbols('libc.so', 'getpid')`.
  Output is capped at 50,000 symbols / 1 MiB and ends with `# truncated` when
  clipped.
- `module.resolve` first tries `dlsym`, then falls back to the ELF symbol
  table, so it can also resolve `LOCAL` (non-exported) symbols when the loaded
  image still contains a full symbol table.
- The same ELF fallback is used by `native.call` and `native.install`: after
  the exported-symbol lookup fails they can target non-exported symbols.
- Enumerating symbols is read-only; the library must be loaded first.
- Unloading while other code still uses the library is unsafe; prefer keeping
  it loaded and only calling `module.unload` for libraries you loaded yourself.

## Examples

Inspect a loaded library:

```sh
./dejavu exec -p com.android.settings \
  -e "return module.symbols('libc.so', 'getpid')"
# getpid	F	GLOBAL	0x...
# __getpid	F	LOCAL	0x...
```

Call a non-exported symbol:

```lua
local source = [[
void dejavu_native_call(void) {
    int (*target)(void) = (void *)(unsigned long long)DEJAVU_NATIVE_CALL_ADDRESS;
    dejavu_native_call_return((long long)target());
}
]]
local pid = native.call('libc.so', '__getpid', source)
```

Load, use and unload a library:

```lua
local id = module.load('libz.so')
local symbols = module.symbols(id, 'inflate')
local version = module.resolve(id, 'zlibVersion')
module.unload(id)
```

## Failure behaviour

All functions raise ordinary Lua errors (visible through the CLI / companion
log) for invalid arguments, permission failures, missing files, `dlopen`
errors and unknown ids. `module.resolve` returns `nil` when the symbol is not
found, which makes it suitable for probing. Native faults caused by calling a
wrong signature are outside the recoverable error model.
