#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME}"
TOOLCHAIN=$(find "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt" -mindepth 1 -maxdepth 1 -type d -print -quit)
OUT=${OUT_DIR:-"$ROOT/out/native-near-edge"}
LIBRARY=${DOBBY_LIBRARY:-"$ROOT/zygisk/out/lsplant-android-arm64-v8a-api26/dobby/libdobby.a"}
FLAGS=()
case "${DOBBY_DIAGNOSTICS:-1}" in
    1) FLAGS+=(-DDEJAVU_CHECK_DIAGNOSTICS) ;;
    0) FLAGS+=(-UDEJAVU_CHECK_DIAGNOSTICS) ;;
    *) echo "DOBBY_DIAGNOSTICS must be 0 or 1" >&2; exit 1 ;;
esac
mkdir -p "$OUT"
"$TOOLCHAIN/bin/aarch64-linux-android26-clang++" -std=c++17 -O2 -Wall -Wextra -Werror \
    -fPIE -pie -static-libstdc++ -Wl,-z,max-page-size=16384 -Wl,--no-undefined \
    -I"$ROOT/zygisk/third_party/Dobby/include" -I"$ROOT/zygisk/third_party/Dobby/source" \
    "${FLAGS[@]}" "$ROOT/zygisk/tests/device/native_near_edge_test.cpp" \
    "$LIBRARY" -llog -ldl -o "$OUT/native-near-edge.unstripped"
cp "$OUT/native-near-edge.unstripped" "$OUT/native-near-edge"
"$TOOLCHAIN/bin/llvm-strip" --strip-unneeded "$OUT/native-near-edge"
echo "Built isolated near-page test in $OUT"
