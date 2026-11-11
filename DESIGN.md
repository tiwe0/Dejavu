# Design

## Source of truth

This document describes the active Dejavu Manager design. The Manager imports
manifest-based Lua, Native and JIT ZIP packages, controls them from the app
and shows declared live events in memory. The implementation is documented in
the [Manager guide](app/README.md).

## Brand
Focused technical utility. Trust comes from a visible target, explicit actions,
truthful runtime state and useful errors. Default to warm ivory and forest green, offer coherent optional palettes, and keep the
pixel dog from the launcher mark.
Avoid decorative dashboards, status inferred from import success and hidden starts.

## Product goals

Select a single-plugin ZIP, validate its manifest and declared resources, and
configure its scope. Use Control to start or load the plugin, inspect its
state and events, and stop or unload it. A complete plugin package can be
imported and controlled entirely from the app.
Non-goals: marketplace, background message archives, automatic app restarts,
updating native module ABI or treating arbitrary folders as executable plugins.

## Personas and jobs
The owner of a rooted Android test device manages their own target applications.
They need repeatable plugin activation and live feedback on a phone screen.

## Information architecture

The three main destinations are Overview, Plugins and Configuration. Overview shows
module detection, aggregate injection targets, root/helper status and project
references. Each plugin owns a multi-process scope. The native injection list
is the union of plugin scopes and legacy/manual targets. Plugins shows compact
identity rows (icon, name, type/version) and a floating ZIP add button. Details
are internal Control / Scope / Info tabs. Control selects the inspected process
and shows its runtime state. Lua-authored pages render inline here. Switching
the inspected process changes the displayed state only.
Scope is multi-select and Info contains manifest metadata and configured targets.
Configuration offers persisted palettes and system/light/dark mode plus log access.
The language picker remains at the far right of the top bar. The mobile console
and global single-target scope screen have been removed.
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
16dp page padding and 48dp minimum action targets. The Overview module panel
shows detection and version with colors from the selected theme. Native, JIT
and Lua summary rows share a rounded surface. List names can span two lines
before truncating. Status includes text as well as color.

## Components

The plugin list uses a shared summary row, a floating add button, an import
progress indicator and error banners. Details reuse TargetMenu, StatusPill
and Material dialogs. Declared events appear in the Lua page or a live-event
sheet. The ZIP picker uses the platform Storage Access Framework.

## Accessibility
Keep touch targets >=48dp, label icon actions, preserve TalkBack reading order,
use text for progress/errors/status and respect platform font scaling. Do not
show streaming text in an accessibility live region that repeatedly interrupts.

## Responsive behavior
Single-column phone layout; sheets scroll with keyboard and large text.
Constrain controls to available width and avoid fixed-height content containers.

## Interaction states

The ZIP picker closes quietly on cancellation. Missing or invalid manifests
surface an import error. Lua runtime status is unknown, stopped, running or
error. Pending start and stop actions show progress and disable conflicting
controls. A failed stop shows an error and leaves cleanup available for retry.
Live events show connection state and keep a bounded list that clears on exit.

## Content voice

English and Simplified Chinese resources with a follow-system option. Use
Start and Stop for Lua, Load and Unload for Native/JIT, and Live events for the
event sheet. Distinguish an imported package from a running plugin.
Explain only actionable prerequisites; no internal stack traces in normal UI.

## Implementation constraints

Kotlin + Jetpack Compose Material 3, minSdk26. No new third-party dependencies.
Each ZIP contains one `manifest.json` at its root or one folder deep. The
library stores each plugin in an immediate subfolder named for its manifest ID.
The manifest uses `schemaVersion: 1` and declares the identity, kind, entry and
resources. Manager reads and validates the ZIP through the document provider,
then stages the declared resources in the root library. Replacement uses a
temporary directory and rollback. The generated Lua RPC script embeds the
runtime resources. Manager consumes the page JSON declared in `ui` locally.
Native/JIT manifests declare `.so`/`.c` entry files and SKILL sidecars. Discover
only manifest folders directly under the default root, skipping Lua package
resources. Import and scan never load code. Before an explicit
native/JIT load, copy the chosen source to the target's private files directory,
pin its SHA256 and SKILL SHA256 in the existing trusted allowlist, and preserve
other allowlist entries. Keep library files root-private; do not relax the
native loader's trusted-allowlist checks.
The process-side Lua entry returns start/stop/status functions accepting a context
with session and read(path). Store lifecycle context per package in the target.
Live events use declared socket or legacy session-filtered logcat transports and in-memory decoding only.
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
