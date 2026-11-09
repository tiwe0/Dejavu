# tiktok-region (Lua plugin package)

Fixes the TikTok feed error caused by CN region detection:

- TikTok registers the device via /service/2/device_register/ with
  carrier_region=CN, mcc_mnc=46000, sim_region=CN (China Mobile SIM).
- The server assigns store_region=cn and returns a server.json with TTNet
  traffic-control rule 3003000 cn_block, which drops /aweme/v2/feed/ locally
  (net::ERR_TTNET_TRAFFIC_CONTROL_DROP, -555; UI shows 出错了/稍后重试).
- This plugin spoofs the region inputs to US so the server returns a config
  without the cn_block rule.

Validated on TikTok 47.6.3 / OnePlus PKX110 / Android 15 with DejaVu v0.8.1
(LSPlant). The hooks use the Lua hook.install API, so this is a Lua package
(kind: lua), not a native/JIT plugin: the native plugin ABI only exposes Dobby
native hooks, not Java/LSPlant hooks.

## Files

- manifest.json - Lua package manifest (entry.lua + SKILL.md).
- entry.lua - start/stop/status: installs/removes the 10 hooks; the first hook
  is a guard on the TikTok-only class X.05DL.
- install-early.sh - deploy/restore the early autoload path (module
  config/init.lua) so hooks run before TikTok's device_register; also deploys
  the package under /data/adb/dejavu/plugins/tiktok-region for the Console.
- README.md - this file.
- SKILL.md - agent-facing usage, verification and rollback.

## Usage

    # early autoload + Console library (recommended)
    ./plugins/tiktok_region/install-early.sh

    # restore the module init.lua backup
    ./plugins/tiktok_region/install-early.sh --restore

    # assemble importable folders under out/plugins
    python3 plugins/prepare-library.py

## Implementation notes

- The first hook (X.05DL.LIZ) doubles as a process guard: class resolution
  fails in any non-TikTok process and the rest of the hooks are skipped.
- Values: mcc_mnc 310260 (T-Mobile US), sim/network country "us", carrier
  "T-Mobile", carrier_region "US".
- Do not add a Locale.getCountry hook: libcore LocaleData/calendar init reads
  the default locale country, and a zh_US mismatch crashes TikTok's
  SimpleDateFormat static initializer.
