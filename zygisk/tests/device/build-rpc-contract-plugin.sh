#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME}"
TOOLCHAIN=$(find "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt" -mindepth 1 -maxdepth 1 -type d -print -quit)
OUT=${OUT_DIR:-"$ROOT/out/rpc-device-regression"}
mkdir -p "$OUT"
"$TOOLCHAIN/bin/aarch64-linux-android26-clang++" -std=c++17 -O2 -Wall -Wextra -Werror \
    -shared -fPIC -fvisibility=hidden -static-libstdc++ -I"$ROOT/include" \
    -Wl,-z,max-page-size=16384 -Wl,--no-undefined \
    -Wl,--version-script,"$ROOT/examples/rpc_echo/exports.map" \
    "$ROOT/zygisk/tests/device/rpc_contract_plugin.cpp" -o "$OUT/libdejavu_rpc_contract.so"
echo "$OUT/libdejavu_rpc_contract.so"
