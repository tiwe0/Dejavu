# Dejavu Manager (Android)

Package: `cafe.ivory.dejavu` · version: `0.2.2`

Kotlin + Jetpack Compose Material 3 control app for Dejavu.

## Build

Requirements:

- JDK 21 (Android Studio JBR works)
- Android SDK platform 36 + build-tools 36.0.0
- `local.properties` with `sdk.dir=...`

```sh
./gradlew :app:assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

AGP 9.2.1 uses built-in Kotlin; there is no separate Kotlin Android plugin.

For a project-local toolchain installed under `out/tooling`, use:

```sh
source out/tooling/env.sh
./gradlew :app:assembleDebug :app:lintDebug -PdejavuSideBySide=true --max-workers=2
```

The optional `dejavuSideBySide` profile builds **Dejavu Dev** as
`cafe.ivory.dejavu.debug`, signed with
`out/tooling/android-user/debug.keystore`. This keeps an installed release and
its data available while testing. Keep this signing file for later Dev updates;
the profile uses the standard Android debug signing credentials.

## What it does

- **Overview** – module version, root access, root helper, total injection targets,
  project source, third-party notices and copyright.
- **Plugins** – import ZIP packages, scan `/data/adb/dejavu/plugins`, configure each
  plugin's multi-process scope, and open its controls or Lua-authored page.
  Within a plugin, select the process to inspect/start/stop; changing that selection
  does not stop plugins already running in other processes. Native folders retain
  load/unload and SKILL.md support.
- **Configuration** – six saved color palettes, Android 12+ wallpaper colors,
  system/light/dark appearance, and live framework logs. The mobile Lua console has been removed;
  desktop `dejavu`/`dejavuctl` Lua RPC remains available.

Plugin details use **Control**, **Scope** and **Info** tabs. The control panel
keeps the inspected process selector compact; scope edits and complete manifest
information have their own internal pages. Leaving Control releases the live
viewer and clears its message list.

### Appearance

Configuration offers Forest, Ocean, Iris, Rose, Amber and Graphite palettes,
plus Android 12+ system wallpaper colors. Each supports system, light or dark
appearance. The main Configuration page shows the current choices; tap a row
to open mode or palette options in a sheet. Changes apply immediately, including
the Overview dog panel, in-app mark colors and system-bar icon contrast, and
are saved in Manager's private preferences across relaunches.

### Back navigation

Android's predictive-back callback animates internal back navigation with a
read-only preview of the destination. Complete a gesture to return, or cancel it
to stay on the current page. Plugin details return to the list, Scope returns to
Control, and logs return to Configuration. The root pages keep system back/home
behavior. Previewing Control never attaches event streams or changes secure-window
flags; normal viewer cleanup happens only on a completed navigation.

### Per-plugin scopes

Each manifest plugin has its own scope, including exact secondary process names
such as `com.example:worker`. Scopes apply to both Lua directory plugins and native
`.so`/JIT `.c` plugins. The effective Zygisk injection list is the union of all
plugin scopes and manually managed CLI targets. Sharing a target across plugins
therefore keeps it injected when one plugin removes it.

Scope ownership is stored as a versioned JSON comment in `config/targets.txt`,
followed by the effective process list. Manager and CLI share a lock and atomically
replace this single file, preserving the native parser's 16 KiB limit. Existing
plain target lists migrate to manual ownership; plugins start with empty scopes
and require explicit assignment. CLI `targets add/remove/set/clear` manage the
manual portion and preserve plugin-owned processes.

Restart affected apps after saving scopes for the Zygisk injection changes to
apply. Saving scopes does not run plugin code automatically: use each plugin's
Start/Load control. Removing a scope does not tear down a live hook; explicitly
stop/unload it or restart the affected app. The currently inspected removed target
stays visible for cleanup, with new starts/actions/event listeners disabled.

### Data path

```text
app UID
  ├─ su -M -c: module.prop, targets.txt, root-helper properties
  └─ abstract Unix socket + 32-byte token:
       /data/adb/modules/dejavu_zygisk/run/<process>.<pid>.rpc
       → in-app agent Lua runtime
```

The app itself is not root. It invokes `su -M -c` for root-only reads/writes and
talks to the agent through its authenticated local socket.

## i18n

- Default: English (`values/strings.xml`)
- Simplified Chinese: `values-zh-rCN/strings.xml`
- The top-right language picker supports English, Simplified Chinese and Follow
  system. Android 13+ shares the choice with system app-language settings.

## Notes

The host regressions do not require an Android device. With JDK and the Kotlin
compiler on PATH, prepare the small host-only `org.json` jar and run:

```sh
mkdir -p out/tooling
curl -fL https://repo.maven.apache.org/maven2/org/json/json/20250517/json-20250517.jar \
  -o out/tooling/json-20250517.jar
make -C zygisk test
bash app/tests/run-host.sh
bash app/tests/importer/run-host.sh
bash app/tests/native/run-host.sh
bash app/tests/scopes/run-host.sh
bash app/tests/archive/run-host.sh
bash app/tests/appearance/run-host.sh
python3 tests/target_scopes_test.py
```

`KOTLINC`, `JAVA_HOME` and `JSON_JAR` can select local tools. Android itself
already provides `org.json`; no application dependency was added. Checks cover
RPC endpoint selection, target changes, root commands, package validation and
Lua lifecycle ownership, a self-contained sample package and live-event frame decoding.

- Debug APK uses a development key; release signing is configured separately.
- Plugin controls require the v0.7.0+ module with the RPC-enabled agent.
- Native `.so`/JIT `.c` ZIP packages can be imported and loaded from the app. Loading
  stages a target-readable copy and pins its SHA256 in the trusted allowlist.

## ZIP plugin import

The default library is `/data/adb/dejavu/plugins/<plugin-id>`. The app reads it
through `su`; it is independent of the installed module's versioned directory.
Tap the floating **+** on the plugin list and select a ZIP containing a valid
`manifest.json` and every resource declared in `files`. The manifest can be at
the ZIP root or inside one enclosing folder. Open the imported plugin, configure
its **Scope**, select a scoped process and tap **Start**. Importing copies code
only and never starts it. The + button hides during downward list scrolling and
reappears when scrolling up or reaching either end.

Use the top-right refresh action to discover folders copied into the default
library outside the app. The library path remains the default scan location;
it is no longer shown as a separate list component.

ZIP parsing happens in memory before any package is staged. Only declared
resources are installed. Missing/invalid manifests, missing resources, conflicting
paths, traversal, duplicates, encrypted/unsupported ZIP formats, damaged archives
and symbolic links are rejected. ZIP input is limited to 80 MiB, with at most
1024 entries and 64 MiB of total inflated content (including unused entries).
Lua packages retain their 256 KiB limit; native/JIT packages retain their 64 MiB
limit. Updating a package preserves the old version if validation or staging fails.
Only immediate subfolders with `manifest.json` are plugins. Bare files, folders
without manifests and the older `dejavu-plugin.json` name are ignored. The
manifest identifier must match its installed folder name.

The generic Lua fixture in `app/tests/fixtures/example-plugin` demonstrates the
package lifecycle and declared resource access. **Live events** keeps at most
50 events in memory;
closing the sheet, leaving the tab or changing target clears them. No message
files or history database are created.

Native and JIT packages use the same metadata fields with `kind: "native"` and
a `.so` entry, or `kind: "jit"` and a `.c` entry. Their declared binary/source
resources are copied without executing them. The local list shows manifest
name and version/type. Its detail page exposes description, identifier and scope count
in **Plugin info**, alongside the inspected target's runtime state. Tap
**Load** to deploy and run one plugin; native/JIT package imports are limited to
32 resources, 32 MiB per library, 512 KiB per JIT entry and 64 MiB in total.
SKILL sidecars must also be declared. Other trusted-plugin entries are preserved.

Each directory package uses this version-1 contract:

```json
{
  "schemaVersion": 1,
  "id": "example",
  "name": "Example",
  "version": "1.0.0",
  "kind": "lua",
  "entry": "app.lua",
  "files": ["app.lua", "hook.c"]
}
```

The entry returns `start(ctx)`, `stop(ctx)` and `status(ctx)` functions.
`ctx.session` identifies the activation and `ctx.read(path)` returns declared
UTF-8 resources. Optional `events` declares `tag`, `marker` and
`format: "urlencoded-json-v1"`. Its optional `titleField`, `contentField`,
`metaField`, `dedupFields`, `dedupFallbackFields` and `dedupIgnoreValues` configure
the generic event viewer; Manager contains no target-app-specific event logic.
Packages are limited to 32 resources / 256 KiB, and RPC scripts to 1 MiB.
Malformed packages are reported without hiding valid packages. Replacing a
running or unknown-state import is blocked; check its status and stop it first.

Imported files use root-only 700/600 permissions. Import stages and validates a
complete package before replacing it; rename failures restore the old directory.
A power loss between the two renames can leave a hidden `.backup-*` directory
for manual recovery. Lua resources use the Lua package lifecycle; native
libraries continue to use the native plugin ABI and unload rules.

### Plugin pages

Author a page as a Lua table and generate its declarative resource before
packing the folder into a ZIP for import.

```sh
lua scripts/compile-plugin-ui.lua path/to/plugin/ui.lua > path/to/plugin/ui.json.tmp &&
mv path/to/plugin/ui.json.tmp path/to/plugin/ui.json
```

Add `"ui": "ui.json"` to the Lua package manifest and list `ui.json` in
`files`. The Manager validates the resource on import and scan and opens it
from **Open page**. It renders Compose components from the JSON, so the page
can open while the selected process is offline.

Version 2 provides the default `monitor` and `controls` layouts. Both show
the exact target, runtime state and a single lifecycle control. The monitor
adds a live event list. For socket events it also shows the bound address and
opens a port settings sheet. Applying a port reconnects the stream and clears
the visible list. The choice is retained in the current Manager session for
the specific plugin and target. Event rows include the local receipt time.

```lua
return {
    version = 2,
    title = {en = "Monitor", zh = "监控"},
    preset = "monitor",
    eventLimit = 50,
    actions = {
        {id = "sample", label = {en = "Send sample", zh = "发送合成事件"}},
    },
}
```

`monitor` requires an event declaration in the manifest. `eventLimit`
defaults to 50 and can be set from 1 to 50. `controls` shows property rows
and groups the declared actions. Each row has a localized `label` and a
`value` from the page resource.

```lua
return {
    version = 2,
    title = "Test controls",
    preset = "controls",
    rows = {
        {label = "Mode", value = "Synthetic data"},
    },
    actions = {
        {id = "sample", label = "Send sample"},
    },
}
```

Version 1 pages continue to render their declared blocks in order.

```lua
return {
    version = 1,
    title = {en = "Monitor", zh = "监控"},
    blocks = {
        {type = "status", bind = "runtime.state"},
        {type = "input", bind = "events.port", label = "Listen port"},
        {type = "button", label = "Refresh", action = "check"},
        {type = "button", label = "Start", action = "start"},
        {type = "button", label = "Stop", action = "stop"},
        {type = "button", label = "Send sample", action = "call", call = "sample"},
        {type = "events", limit = 50},
    },
}
```

The Lua entry can expose named callbacks as `actions = {sample = function(ctx)
... end}`. A version-2 action or a version-1 call button dispatches its
declared name to the running package session in the selected process.
An optional version-1 `text` block displays plain or localized text.
Localized labels use `{en = "...", zh = "..."}`.

For the listener input, declare `"events": {"format": "hook-socket-json-v1",
"topic": "example.message"}` in the manifest. The Manager binds to
`127.0.0.1` and accepts port 0 for automatic allocation or 1024–65535 for a
fixed port. The agent must support `hook.stream_acquire` and
`hook.stream_release`. The hook emits a UTF-8 JSON payload on the topic
`example.message.<ctx.session>` through the existing Hook C event ABI. The
Manager matches the session topic and displays the latest 1–50 events in
memory. Closing the page, switching targets or backgrounding Manager clears
the list and releases the listener. The socket transport carries message
bodies directly to Manager and creates no message files or logcat entries.
The v1 listener is intended for trusted local development. It filters records
by session topic after accepting the TCP connection. A different local process
that connects to a fixed port first can occupy the connection.


## Scope editor

Open a plugin's **Scope** page to select multiple installed apps or add an exact
process name (including `:worker` suffixes). Search by application label or process
name and save the complete selection. Each plugin detail then lets you choose one
of its scoped processes for the displayed runtime state and controls.

## Interaction notes

- The last inspected target is remembered per plugin for the current Manager session.
- Native plugin unload asks for confirmation; scope edits require explicit Save.
- Plugin list refreshes when the Plugins tab opens or the selected target changes.
- Live logs pause auto-scroll when the toggle is off; the filter is local-only.
- The header progress bar reflects root-shell/RPC work in progress.
- `./gradlew` wrapper is pinned to Gradle 9.4.1; use JDK 21 and Android SDK 36.


## Release build

A signed release build is configured but the keystore is intentionally not
committed.

```sh
cp app/keystore.properties.example app/keystore.properties
# create a local keystore:
keytool -genkeypair -v -keystore out/tooling/dejavu-console.jks \
  -alias dejavu-console -keyalg RSA -keysize 2048 -validity 10000 \
  -storepass change-me -keypass change-me \
  -dname "CN=Dejavu Console, O=Dejavu, C=CN"
# edit app/keystore.properties to match
./gradlew :app:assembleRelease
adb install -r app/build/outputs/apk/release/app-release.apk
```

Release uses R8 + resource shrinking. `versionName` is `0.2.2` (`versionCode` 4).
The app is self-signed; for distribution replace the local keystore with a
stable release key.


## Root permission screen

Until KernelSU/Magisk authorizes the app, the permission page replaces the dashboard
with a dedicated permission screen. It requests root, offers a direct shortcut
to KernelSU, and shows the authorization steps. The bottom navigation and
normal dashboard are hidden in this state.

![Permission screen](docs/permission.png)
