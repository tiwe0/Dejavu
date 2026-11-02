# Design

## Source of truth
Status: Active. Updated: 2026-11-02. Scope: Dejavu Console plugin management.
Evidence: app/README.md, app/docs/plugins.png, MainActivity.kt, Screens.kt,
ConsoleUiState.kt, Theme.kt, generic test fixtures, and the native plugin ABI docs.
User requirement: import manifest-based Lua, native and JIT plugin folders;
control them from the app and show declared events live without history files.

## Brand
Focused technical utility. Trust comes from a visible target, explicit actions,
truthful runtime state and useful errors. Keep the existing subdued green theme.
Avoid decorative dashboards, status inferred from import success and hidden starts.

## Product goals
Select a folder once, import its declared code resources, select a target, start,
inspect status/events and stop. Preserve existing native plugin management.
Success: a complete manifest folder imports and can be controlled without
typing device paths or using the host CLI. Importing alone never runs code.
Non-goals: marketplace, background message archives, automatic app restarts,
updating native module ABI or treating arbitrary folders as executable plugins.

## Personas and jobs
The owner of a rooted Android test device manages their own target applications.
They need repeatable plugin activation and live feedback on a phone screen.

## Information architecture
Keep the current five tabs. The Plugins tab shows the selected target, an import
folder action, imported Lua packages, then locally available native/JIT plugins
with their selected-target runtime status. Other process-registered plugins
remain visible below the library.
Directory cards offer Start/Stop, status and live events when declared.
Manifest and declared resources live in `/data/adb/dejavu/plugins/<id>` and the
app scans this root-owned directory by default. Runtime state
comes from the selected process, with explicit disconnected/unknown states.

## Design principles
Bind operations and pending dialogs to their original target. Refresh is not an
empty successful result. Keep source import separate from execution. Validate a
complete package before replacing an existing import. Do not erase a usable
version on failed import. Stop/cleanup errors remain visible and retryable.

## Visual language
Reuse MaterialTheme colorScheme, typography and shapes from Theme.kt, with
16dp page padding, 8–12dp gaps and 48dp minimum action targets. Native and directory
cards share the existing outlined surface treatment. Long names and paths wrap
or appear in a selectable details view. Status includes text as well as color.

## Components
Reuse TargetMenu, EmptyState, StatusPill, outlined cards and Material dialogs.
Add a directory-plugin card, import progress/error area and a live-event sheet.
Use existing Compose dependencies and platform Storage Access Framework.

## Accessibility
Keep touch targets >=48dp, label icon actions, preserve TalkBack reading order,
use text for progress/errors/status and respect platform font scaling. Do not
show streaming text in an accessibility live region that repeatedly interrupts.

## Responsive behavior
Single-column phone layout; sheets scroll with keyboard and large text.
Constrain controls to available width and avoid fixed-height content containers.

## Interaction states
Import: idle/selecting/validating/importing/success/error. User cancellation is
quiet. Missing or invalid manifest gives a concrete folder-selection error.
Runtime: unknown/stopped/starting/running/stopping/error. Prevent repeated actions
and stale-target results. A failed stop retains running/unknown state for retry.
Events: connecting/listening/empty/error; bounded memory and clear-on-dismiss.

## Content voice
English and Simplified Chinese resources. Use Import folder, Start, Stop and
Live events consistently. Distinguish an imported package from a running plugin.
Explain only actionable prerequisites; no internal stack traces in normal UI.

## Implementation constraints
Kotlin + Jetpack Compose Material 3, minSdk26. No new third-party dependencies.
Directory contract: one immediate subfolder per plugin, with a required
manifest.json schemaVersion1, identifier/name/version, kind, entry and explicitly
declared resources. Missing manifests, bare files and legacy manifest names do
not create plugins. Reject path traversal, duplicate names,
oversized packages and unsupported kinds. Read sources via the document provider,
stage/validate them, then write the root library through the existing root shell.
Use temporary staging plus rollback when replacing a package. Do not require
general storage access. Root directories are private; Lua resources are embedded
in the authenticated RPC script so targets need no direct root-directory access.
Native/JIT manifests declare `.so`/`.c` entry files and SKILL sidecars. Discover
only manifest folders directly under the default root, skipping Lua package
resources. Import and scan never load code. Before an explicit
native/JIT load, copy the chosen source to the target's private files directory,
pin its SHA256 and SKILL SHA256 in the existing trusted allowlist, and preserve
other allowlist entries. Keep library files root-private; do not relax the
native loader's trusted-allowlist checks.
The process-side Lua entry returns start/stop/status functions accepting a context
with session and read(path). Store lifecycle context per package in the target.
Live events use session-filtered logcat frames and in-memory decoding only.
Event display fields and deduplication identity are declared in the plugin's
manifest; Manager has no application-specific pages, fields or event markers.
Tests cover validation, source quoting, lifecycle ownership and frame limits;
full APK/device UI checks are required when an Android SDK is available.
Host importer tests exercise the real shell staging/scan/rollback against an
isolated mapped filesystem and an in-memory document provider. The generated
App Lua loader has also passed an isolated device lifecycle test.

## Open questions
- [x] JDK 21, Android SDK 36/Build Tools 36.0.0 and Gradle 9.4.1 are installed
  under out/tooling. Full side-by-side APK compilation and lint completed.
- [x] Lua, native and JIT plugins share the required manifest.json metadata
  contract; imported native binaries retain their bytes. Device-path loading
  remains available for already deployed plugins.
