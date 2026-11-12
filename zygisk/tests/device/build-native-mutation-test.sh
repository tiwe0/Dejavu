#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME}"
TOOLCHAIN=$(find "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt" -mindepth 1 -maxdepth 1 -type d -print -quit)
OUT=${OUT_DIR:-"$ROOT/out/native-mutation"}
LIBRARY=${DOBBY_LIBRARY:-"$ROOT/zygisk/out/lsplant-android-arm64-v8a-api26/dobby/libdobby.a"}
mkdir -p "$OUT"
"$TOOLCHAIN/bin/aarch64-linux-android26-clang++" -std=c++17 -O2 -Wall -Wextra -Werror \
    -fPIE -pie -static-libstdc++ -Wl,-z,max-page-size=16384 -Wl,--no-undefined \
    -I"$ROOT/zygisk/third_party/Dobby/include" -I"$ROOT/zygisk/third_party/Dobby/source" \
    "$ROOT/zygisk/tests/device/native_mutation_test.cpp" "$LIBRARY" -llog -ldl \
    -Wl,--wrap=mmap -Wl,--wrap=mprotect -Wl,--wrap=munmap \
    -o "$OUT/native-mutation.unstripped"
cp "$OUT/native-mutation.unstripped" "$OUT/native-mutation"
"$TOOLCHAIN/bin/llvm-strip" --strip-unneeded "$OUT/native-mutation"
echo "Built isolated mutation test in $OUT"
