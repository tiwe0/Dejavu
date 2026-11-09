# tiktok-region plugin skill

Use this plugin when TikTok (com.zhiliaoapp.musically) shows feed errors
("出错了 / 稍后重试" or "网络不稳定") because the device was registered in the CN
region and TikTok received a server.json containing TTNet rule 3003000
cn_block, which drops /aweme/v2/feed/ locally with
net::ERR_TTNET_TRAFFIC_CONTROL_DROP (net_error -555, statusCode -1002).

## What it does

Installs 10 LSPlant Java hooks that force the TikTok device_register region
inputs to US:

- X.05DL.LIZ / LIZJ / LIZLLL / LJ -> "US" / "310260" / "T-Mobile" / "US"
- TelephonyManager getSimCountryIso / getNetworkCountryIso -> "us"
- TelephonyManager getSimOperator / getNetworkOperator -> "310260"
- TelephonyManager getSimOperatorName / getNetworkOperatorName -> "T-Mobile"

The first hook is a guard: X.05DL only exists in TikTok, so other target
processes fail class resolution and install nothing.

## When to start it

Region is decided at device_register time, i.e. early in process start. Two
ways to run it:

1. Early autoload (recommended): run plugins/tiktok_region/install-early.sh.
   It appends a generated loader to the module config/init.lua, so hooks are
   installed inside Application.attach before TikTok fetches its region config,
   and it also deploys this package to /data/adb/dejavu/plugins/tiktok-region
   for the DejaVu Console library.
2. Manual start from the DejaVu Console plugin package lifecycle. This installs
   the hooks in the already-running process; it does not replay an earlier
   device_register. It is useful for re-applying the spoof or for testing.

## Verification

- logcat tag DejavuHook: "tiktok-region: installed 10/10 hooks", then
  "tiktok-region hit ..." lines while TikTok starts.
- /data/user/0/com.zhiliaoapp.musically/shared_prefs/ttnet_store_region.xml:
  store_region should become "us".
- files/server.json should no longer contain cn_block / rule 3003000.
- The feed shows videos instead of the error view.

## Rollback

plugins/tiktok_region/install-early.sh --restore restores the module init.lua
backup and restarts the target. Stopping the plugin from the Console removes
the hooks for the current process only.

## Notes

- Tested with TikTok 47.6.3 (versionCode 2024706030) on Android 15 arm64 with
  DejaVu v0.8.1. Obfuscated class X.05DL may change between app versions;
  relocate the new device_register carrier/mcc_mnc/sim_region sources before
  reusing on a new version.
- Do not hook java.util.Locale.getCountry as a shortcut: it breaks Android ICU
  locale/calendar initialization and crashes TikTok with
  ExceptionInInitializerError key [und_aaland] in alias:language.
- Personal research tooling for a device you own; TikTok may treat the spoofed
  environment as a risk signal server-side.
