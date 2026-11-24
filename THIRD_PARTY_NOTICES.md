# Third-Party Notices

Dejavu itself is released under the MIT License (see `LICENSE`). It embeds,
links, or bundles the third-party components below; each component remains
under its own license. The Zygisk module package ships `LICENSE`, this file,
and the component license texts that are available in the working tree under
`LICENSES/`.

| Component | Version / pin | License | Upstream source | License text |
| --- | --- | --- | --- | --- |
| Lua | 5.4.8 (`extern/lua` @ `6e22fedb`) | MIT | <https://github.com/lua/lua> | `extern/lua/lua.h` (copyright notice at end of file) |
| TinyCC (Frida fork) | 0.9.27-frida (`extern/tinycc` @ `ca5ee011`) | LGPL-2.1 | <https://github.com/frida/tinycc> | `extern/tinycc/COPYING` |
| LSPlant | main @ `6193e25` | LGPL-3.0 | <https://github.com/LSPosed/LSPlant> | fetched: `zygisk/third_party/LSPlant/LICENSE` |
| DexBuilder | `ac7fb223` | LGPL-3.0 | <https://github.com/LSPosed/DexBuilder> | fetched: `.../dex_builder/LICENSE` |
| parallel-hashmap | `0cd57d29` | Apache-2.0 | <https://github.com/greg7mdp/parallel-hashmap> | fetched: `.../parallel_hashmap/LICENSE` |
| Dobby | `5dfc854` | Apache-2.0 | <https://github.com/jmpews/Dobby> | fetched: `zygisk/third_party/Dobby/LICENSE` |
| KCP | vendored (`zygisk/third_party/kcp`) | MIT | <https://github.com/skywind3000/kcp> | `zygisk/third_party/kcp/LICENSE` |
| XZ Embedded | `52f2742f` (vendored) | 0BSD / Public Domain | <https://github.com/tukaani-project/xz-embedded> | `zygisk/third_party/xz-embedded/COPYING` |
| cJSON | 1.7.19 (vendored) | MIT | <https://github.com/DaveGamble/cJSON> | `router/third_party/cJSON/LICENSE` |
| libevent | 2.1.12-stable (SHA-256 pinned archive) | BSD-3-Clause and bundled notices | <https://github.com/libevent/libevent> | fetched: `router` build dependency `LICENSE` |
| Mbed TLS | 3.6.3 (SHA-256 pinned archive) | Apache-2.0 or GPL-2.0-or-later | <https://github.com/Mbed-TLS/mbedtls> | fetched: `router` build dependency `LICENSE` |
| Zygisk API header | API v5 @ `8ce26128` | 0BSD | <https://github.com/topjohnwu/zygisk-module-sample> | `zygisk/include/zygisk.hpp` (header) |
| JetBrains Mono NL (Manager font, Regular/Medium) | 2.304 | OFL-1.1 | <https://github.com/JetBrains/JetBrainsMono> | `LICENSES/JetBrainsMono-OFL.txt` |

## Source availability and patches

- The pinned components are fetched by commit and verified by SHA-256 in
  `zygisk/scripts/fetch-third-party.sh`.
- The vendored TinyCC sources are reconstructed from the pinned submodule plus
  `patches/tinycc-runtime-safety.patch` at build time; see
  `patches/README.md`.
- Dobby is built from source with
  `zygisk/patches/dobby-native-publication.patch` (see
  `zygisk/third_party/README.md`).
- LSPlant is additionally patched by
  `zygisk/patches/lsplant-main-reflection-shorty.patch` (see
  `zygisk/third_party/README.md`).
- LGPL-3.0 components (LSPlant, DexBuilder) are statically linked. Their
  corresponding source is available at the pinned upstream commits above, and
  the project can be relinked by rebuilding from this repository and the
  pinned dependencies.

## Notes

- This list is provided for attribution and convenience; it is not legal
  advice. When redistributing binaries, verify the obligations of each license.
- Dobby, LSPlant and Zygisk are independent projects and are not affiliated
  with Dejavu.
