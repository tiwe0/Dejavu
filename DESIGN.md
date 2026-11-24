# Design

## Source of truth

This document describes the active Dejavu Manager design. The Manager imports
manifest-based Lua, Native and JIT ZIP packages, controls them from the app
and shows declared live events in memory. The implementation is documented in
the [Manager guide](app/README.md).

## Brand
Focused technical utility. Trust comes from a visible target, explicit actions,
truthful runtime state and useful errors. Default to warm ivory and forest green, offer coherent optional palettes, and keep the
pixel dog from the launcher mark (top bar of Overview and the empty scope state).
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

The three main destinations are Overview, Plugins and Configuration. Overview
leads with the injection chain (module → root → helper): each link renders one
of three states (Healthy, Pending, or Broken), with a dashed connector and an
inline repair action directly underneath when a link needs attention (manage
module, reboot pending, or re-check helper). Below it, "Injection scope" groups
configured plugins by target process (app label, monospace process name, plugin
rows with a type tag); processes injected without a plugin scope are listed as
manual targets and can be removed inline with confirmation. Overview never shows
per-process running state because only the selected process is read. Each plugin
owns a multi-process scope. The native injection list is the union of plugin
scopes and legacy/manual targets. Plugins shows identity rows (monogram, name,
`TYPE · version` tag, scope count) on hairline dividers and an extended "Import
ZIP" button. Details keep the inspected-process selector above a Control / Scope
/ Info sibling segmented control on every tab; pressing Back returns directly to
the originating destination (Overview or Plugins), prompting Save or Discard if
Scope has unsaved edits. Control selects the inspected process and shows its
runtime state; plugins without a declared page get an enlarged state block and a
bottom-docked primary action. Constructive actions (`Start`, `Load`, `Check
status`) use filled buttons, while destructive lifecycle actions (`Stop`,
`Unload`) consistently use error-outlined buttons across Plugin Detail, Lua
pages and Router. Lua-authored pages render inline here. Switching the inspected
process changes the displayed state only.
Scope is multi-select and Info contains manifest metadata and configured targets.
Configuration offers persisted palettes and system/light/dark mode, diagnostics
(installer, logs, router) and About (source, third-party notices, license).
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

North star: "see which plugin is injected into which process, and which link of
the chain is broken". Reuse MaterialTheme colorScheme, typography and shapes
from Theme.kt, with 16dp page padding and 48dp minimum action targets. Forest is
warm ivory (`#F6F3EA` background, `#FCFAF5` surface) with forest green
(`#1F5C43`) for primary actions and selection. A separate signal green
(`ColorScheme.signal`) is reserved for live state: healthy chain links and
running plugins. Unknown or stopped states use a neutral dot; errors use the
error color. Shapes are 6/10/12/16dp; groups are outlined surfaces with
hairline dividers instead of filled cards. Identifiers (package and process
names, plugin IDs, versions, ports, socket paths) use the bundled JetBrains Mono
NL (`DejavuMono`, `Typography.mono`) because some ROMs replace the system
monospace face. Status always includes text as well as color. Expected
preconditions such as an offline target use a neutral notice, not an error
banner. List names can span two lines before truncating.

Motion (`ui/Motion.kt`, one vocabulary for the whole app):
- Drawer feel for surfaces: damped spring (ratio 0.92, stiffness 420), no visible
  overshoot. Exits run at about two thirds of the enter duration and accelerate.
- Pages: entering a plugin or tool detail pushes in from the trailing edge
  (1/5 width) while the list drifts back; returning is the exact reverse.
  Bottom-navigation siblings fade through with a slight rise. A committed
  predictive-back gesture skips the page animation so it never plays twice;
  the outgoing page renders as a read-only preview while it leaves.
- Detail tabs: the segmented plate slides to the chosen option and content
  moves in the same direction.
- Chrome: the title rolls up to the new name, the back arrow slides in, the
  bottom bar slides away on detail pages, and loading bars fade.
- Lists: rows animate insertion, removal and reordering; Overview and Plugins
  stagger in once per session (50ms steps, at most 8 steps).
- Feedback: primary actions and the import button scale to 0.97 on press with a
  short spring; runtime state and action labels roll to their new value; the
  progress spinner expands into the button; the chain dims on refresh and
  lights node by node; its repair action expands in place; the running dot
  pulses (paused when backgrounded); palette and light/dark changes cross-fade
  over 280ms across all Material 3 tokens and `ColorScheme.signal`.
- All specs follow the system animator duration scale, so "remove animations"
  makes every state land immediately with the same content.

## Components

Shared pieces live in `ui/ChainComponents.kt` and `ui/Components.kt`:
`InjectionChain`, `LiveStatus` (dot + text, pulsing when live), `TypeTag`,
`PluginMonogram`, `NoticeBanner`, `ErrorBanner`, `TargetMenu`, `ManagerGroup`,
and `ManagerLinkRow`. `ManagerMark` renders with cached Canvas paths and a
subtle contrast badge in light mode. Details reuse `SlidingSegmented` and
Material dialogs. Declared events appear in the Lua page or a `FLAG_SECURE`
live-event sheet. The ZIP picker uses the platform Storage Access Framework.
Scope editing docks its save button at the bottom like plugin actions and
scrolls its header inside the list so landscape and soft-keyboard (`adjustResize`
+ `imePadding()`) layouts remain usable.

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

Kotlin + Jetpack Compose Material 3, minSdk26. No new third-party library
dependencies; the bundled JetBrains Mono NL font files are OFL-1.1 (see
`THIRD_PARTY_NOTICES.md`).
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
