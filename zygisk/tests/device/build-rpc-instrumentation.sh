#!/usr/bin/env bash
set -euo pipefail

# Build the Manager first. This runner uses framework Instrumentation, without JUnit.
repo=$(cd "$(dirname "$0")/../../.." && pwd)
source "$repo/out/tooling/env.sh"
build_tools="$ANDROID_HOME/build-tools/36.0.0"
android_jar="$ANDROID_HOME/platforms/android-36/android.jar"
app_classes="$repo/app/build/intermediates/built_in_kotlinc/debug/compileDebugKotlin/classes"
stdlib=$(find "$GRADLE_USER_HOME/wrapper/dists" -name 'kotlin-stdlib-*.jar' -print -quit)
work=$(mktemp -d "${TMPDIR:-/tmp}/dejavu-rpc-instrumentation.XXXXXX")
output=${1:-"$repo/out/rpc-instrumentation.apk"}
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/classes" "$work/dex" "$(dirname "$output")"
javac --release 17 -cp "$android_jar:$app_classes:$stdlib" -d "$work/classes" \
    "$repo/zygisk/tests/device/RpcAndroidSmoke.java" \
    "$repo/zygisk/tests/device/RpcAndroidInstrumentation.java"
jar cf "$work/runner.jar" -C "$work/classes" .
"$build_tools/d8" --min-api 26 --lib "$android_jar" \
    --classpath "$app_classes" --classpath "$stdlib" \
    --output "$work/dex" "$work/runner.jar"
cp "$repo/zygisk/tests/device/RpcAndroidSmokeManifest.xml" "$work/AndroidManifest.xml"
"$build_tools/aapt2" link -I "$android_jar" --manifest "$work/AndroidManifest.xml" \
    -o "$work/unsigned.apk"
(cd "$work/dex" && zip -q "$work/unsigned.apk" classes.dex)
"$build_tools/zipalign" -f 4 "$work/unsigned.apk" "$work/aligned.apk"
"$build_tools/apksigner" sign --ks "$ANDROID_USER_HOME/debug.keystore" \
    --ks-key-alias androiddebugkey --ks-pass pass:android --key-pass pass:android \
    --out "$output" "$work/aligned.apk"
"$build_tools/apksigner" verify "$output"
echo "Built $output"
echo 'Install with adb install -r, then run:'
echo 'adb shell am instrument -w -r cafe.ivory.dejavu.rpcsmoke/dejavu.tests.RpcAndroidInstrumentation'
