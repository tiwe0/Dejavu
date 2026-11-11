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
- `sign` -> load `files/ttrpc.dex` with InMemoryDexClassLoader and invoke
  `ttrpc.Rpc.sign(url, headersJson)`, returning the real `direct.headers`
  (X-Gorgon/X-Khronos/X-Argus/X-Ladon)

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

## Signing

The signer requires `ttrpc.dex` in the app files directory:

```text
/data/user/<user>/com.zhiliaoapp.musically/files/ttrpc.dex
```

The dex is loaded in-process and calls the app's real signing path. No
fixed/fake signature fallback is used.

## Current scope

Credential synchronization and real request signing are implemented. The
server-side executor/gateway enablement remains a deployment task.