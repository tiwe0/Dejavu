#!/usr/bin/env bash
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../.." && pwd)
source "$repo/out/tooling/env.sh"
build_tools="$ANDROID_HOME/build-tools/36.0.0"
android_jar="$ANDROID_HOME/platforms/android-36/android.jar"
fixture="$repo/zygisk/tests/device/jni_thread_scope"
work=$(mktemp -d "${TMPDIR:-/tmp}/dejavu-jni-scope.XXXXXX")
output=${1:-"$repo/out/lua-rpc/jni-thread-scope-test.apk"}
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/classes" "$work/dex" "$work/native/lib/arm64-v8a" "$(dirname "$output")"
jni_scope_toolchain=$(find "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt" -mindepth 1 -maxdepth 1 -type d -print -quit)
"$jni_scope_toolchain/bin/aarch64-linux-android26-clang++" -std=c++17 -O2 -Wall -Wextra -Werror \
    -fPIC -shared -static-libstdc++ -fvisibility=hidden -I"$repo/zygisk/include" \
    -Wl,--no-undefined -Wl,-z,max-page-size=16384 \
    "$fixture/jni_thread_scope_test.cpp" -o "$work/native/lib/arm64-v8a/libdejavu_jni_scope_test.so"
"$jni_scope_toolchain/bin/llvm-strip" --strip-unneeded "$work/native/lib/arm64-v8a/libdejavu_jni_scope_test.so"
javac --release 17 -cp "$android_jar" -d "$work/classes" "$fixture/JniThreadScopeInstrumentation.java"
jar cf "$work/runner.jar" -C "$work/classes" .
"$build_tools/d8" --min-api 26 --lib "$android_jar" --output "$work/dex" "$work/runner.jar"
"$build_tools/aapt2" link -I "$android_jar" --manifest "$fixture/AndroidManifest.xml" -o "$work/unsigned.apk"
(cd "$work/dex" && zip -q "$work/unsigned.apk" classes.dex)
(cd "$work/native" && zip -q "$work/unsigned.apk" lib/arm64-v8a/libdejavu_jni_scope_test.so)
"$build_tools/zipalign" -f 4 "$work/unsigned.apk" "$work/aligned.apk"
"$build_tools/apksigner" sign --ks "$ANDROID_USER_HOME/debug.keystore" --ks-key-alias androiddebugkey \
    --ks-pass pass:android --key-pass pass:android --out "$output" "$work/aligned.apk"
"$build_tools/apksigner" verify "$output"
echo "Built $output"
