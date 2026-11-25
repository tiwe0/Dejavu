# SQLite3 Amalgamation

Vendored from the official SQLite 3.54.1 amalgamation release:

- Source: <https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip>
- Archive SHA3-256 (official sqlite.org): `92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30`
- Archive SHA-256: `0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9`
- License: Public Domain (<https://www.sqlite.org/copyright.html>)

Compiled directly into `libdejavu.so` with hidden symbol visibility (`-fvisibility=hidden`)
and `-Wl,--gc-sections` so Dejavu's Lua `sqlite` module works identically across Android
API 26–37 (without depending on private platform `libsqlite.so`) and host Linux/macOS tests.
