# Dejavu Console (Android)

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

## What it does

- **Overview** – module version, root shell, root helper endpoint, selected target.
- **Targets** – edit `config/targets.txt`, restart target processes.
- **Plugins** – reads `plugin.list()` through Dejavu's authenticated Lua RPC,
  shows loaded plugins, SKILL.md, unload and load-by-device-path.
- **Live logs** – streams `DejavuZygisk`, `DejavuAgent`, `DejavuPlugin`,
  `DejavuHook`, `DejavuNative`.
- **Lua console** – executes Lua on the selected target via the same control
  channel used by `dejavuctl`.

### Data path

```text
app UID
  ├─ su -c: module.prop, targets.txt, root-helper properties
  └─ abstract Unix socket + 32-byte token:
       /data/adb/modules/dejavu_zygisk/run/<process>.<pid>.rpc
       → in-app agent Lua runtime
```

The app itself is not root. It invokes `su -c` for root-only reads/writes and
talks to the agent through its authenticated local socket.

## i18n

- Default: English (`values/strings.xml`)
- Simplified Chinese: `values-zh-rCN/strings.xml`
- Android 13+ per-app locale via `res/xml/locales_config.xml`
- In-app language picker in the top bar (Android 13+)

## Notes

- Debug APK is unsigned for release distribution.
- The console requires the v0.7.0+ module with the RPC-enabled agent.
- Plugin installation from host files is intentionally not exposed; use
  `dejavu plugin install` on the host, then load by device path in the app.


## Screens

| | |
|---|---|
| ![Overview](docs/home.png) | ![Targets](docs/targets.png) |
| ![Plugins](docs/plugins.png) | ![Live logs](docs/logs.png) |

![Lua console](docs/console.png)

## Interaction notes

- Selected target is persisted in SharedPreferences and reused after restart.
- Destructive operations (remove target, unload plugin) ask for confirmation.
- Plugin list refreshes when the Plugins tab opens or the selected target changes.
- Live logs pause auto-scroll when the toggle is off; the filter is local-only.
- The header progress bar reflects root-shell/RPC work in progress.
- `./gradlew` wrapper is pinned to Gradle 9.4.1; use JDK 21 and Android SDK 36.
