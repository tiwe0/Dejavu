# X Agent plugin

Loaded inside `com.twitter.android` by DejaVu.

The plugin:

- reads the current X account id from the app's shared preferences;
- connects outbound to `127.0.0.1:39111` (dejavurouter);
- registers route = account id;
- serves `token`, `sign`, and `ping`;
- uses AndroidKeyStore alias `twitter-android-keystore-alias-<account>`;
- never uses HTTP.

RPC examples:

```json
{"type":"request","id":"j1","route":"<account>","method":"token","params":{}}
{"type":"request","id":"j2","route":"<account>","method":"sign","params":{"digest":"<base64>"}}
```

Responses echo the route. If the route does not match the detected account, the
plugin returns `route_mismatch`.
