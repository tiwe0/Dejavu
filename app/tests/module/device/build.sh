#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
source "$repo/out/tooling/env.sh"
build_tools="$ANDROID_HOME/build-tools/36.0.0"
android_jar="$ANDROID_HOME/platforms/android-36/android.jar"
work=$(mktemp -d "${TMPDIR:-/tmp}/dejavu-module-instrumentation.XXXXXX")
output=${1:-"$repo/out/module-instrumentation.apk"}
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/classes" "$work/dex" "$(dirname "$output")"
javac --release 17 -Xlint:all -cp "$android_jar" -d "$work/classes" \
    "$repo/app/tests/module/device/ModuleAndroidInstrumentation.java"
jar cf "$work/runner.jar" -C "$work/classes" .
"$build_tools/d8" --min-api 26 --lib "$android_jar" --output "$work/dex" "$work/runner.jar"
"$build_tools/aapt2" link -I "$android_jar" \
    --manifest "$repo/app/tests/module/device/AndroidManifest.xml" -o "$work/unsigned.apk"
(cd "$work/dex" && zip -q "$work/unsigned.apk" classes.dex)
"$build_tools/zipalign" -f 4 "$work/unsigned.apk" "$work/aligned.apk"
"$build_tools/apksigner" sign --ks "$ANDROID_USER_HOME/debug.keystore" --ks-key-alias androiddebugkey \
    --ks-pass pass:android --key-pass pass:android --out "$output" "$work/aligned.apk"
"$build_tools/apksigner" verify "$output"
echo "Built $output"
echo 'Component: cafe.ivory.dejavu.modulesmoke/dejavu.tests.ModuleAndroidInstrumentation'
echo 'Argument: -e control_socket <unique-abstract-name>'
echo 'Protocol: BUSY -> RELEASED -> READY -> INSTALL -> PENDING -> CANCELLED -> PASS'
