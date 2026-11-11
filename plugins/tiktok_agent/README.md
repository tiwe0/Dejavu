# TikTok Agent

Phone-side credential agent for the TikTok Android app
(`com.zhiliaoapp.musically`).

It reads the current Android user's TikTok credential files:

```text
/data/user/<user>/com.zhiliaoapp.musically/shared_prefs/ttnetCookieStore.xml
/data/user/<user>/com.zhiliaoapp.musically/shared_prefs/aweme_user.xml
/data/user/<user>/com.zhiliaoapp.musically/shared_prefs/applog_stats.xml
```

and connects to dejavurouter with route = TikTok `user_id`.

Methods:

- `ping` -> plugin/account/version
- `credentials` -> account id, username, device id, install id, cookie list
- `refresh` -> same as credentials, re-read from disk

The cookie store is Java-serialized hex inside SharedPreferences. The plugin
extracts the cookie name/value pairs directly and does not need to instantiate
the app's cookie classes.

## Deployment

Build:

```bash
./plugins/tiktok_agent/build.sh
```

Then deploy the resulting `libdejavu_tiktok_agent.so` into each Android user's
TikTok app files, add its path/hash to the DejaVu plugin allowlist, and start
TikTok so the plugin loads. A deploy helper can be added next.

## Current scope

This is credential synchronization only. TikTok request signing
(X-Gorgon/X-Argus/X-Khronos and related headers) is not implemented here.
