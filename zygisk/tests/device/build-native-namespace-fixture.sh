#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
: "${ANDROID_HOME:?Set ANDROID_HOME}"
: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME}"
TOOLS="$ANDROID_HOME/build-tools/${ANDROID_BUILD_TOOLS_VERSION:-36.0.0}"
TOOLCHAIN=$(find "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt" -mindepth 1 -maxdepth 1 -type d -print -quit)
OUT=${OUT_DIR:-"$ROOT/out/native-namespace-fixture"}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/dejavu-native-namespace.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/classes" "$WORK/dex" "$OUT"
javac --release 17 -Xlint:all -Werror -d "$WORK/classes" \
    "$ROOT/zygisk/tests/device/java/io/dejavu/bridge/NativeNamespaceFixture.java"
jar cf "$WORK/fixture.jar" -C "$WORK/classes" .
"$TOOLS/d8" --min-api 26 --lib "$ANDROID_HOME/platforms/android-36/android.jar" \
    --output "$WORK/dex" "$WORK/fixture.jar"
cp "$WORK/dex/classes.dex" "$OUT/fixture.dex"
"$TOOLCHAIN/bin/aarch64-linux-android26-clang++" -std=c++17 -O2 -Wall -Wextra -Werror \
    -shared -fPIC -fvisibility=hidden -static-libstdc++ \
    -Wl,-z,max-page-size=16384 -Wl,--no-undefined \
    "$ROOT/zygisk/tests/device/native_namespace_fixture.cpp" -o "$OUT/libdejavu_native_namespace.so"
cat > "$OUT/replacement.c" <<'C'
long long dejavu_native_target(long long value) {
    long long (*original)(long long) = (void *)dejavu_native_original();
    if (!original) return -90001;
    dejavu_native_hit();
    return original(value) + 100;
}
C
cp "$ROOT/zygisk/tests/device/native_namespace_hook.lua" "$OUT/native_namespace_hook.lua"
echo "Built namespace fixture in $OUT"
