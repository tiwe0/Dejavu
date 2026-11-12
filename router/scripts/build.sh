#!/usr/bin/env bash
# Standalone host or Android build. All generated files stay under out/.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
MODE=${1:-host}
case "$MODE" in
    host) OUT=${OUT_DIR:-"$ROOT/out/router-host"}; EXTRA=(-DDEJAVU_ROUTER_SYSTEM_LIBEVENT=OFF) ;;
    android)
        : "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to Android NDK r29 or newer}"
        OUT=${OUT_DIR:-"$ROOT/out/router-android-arm64"}
        EXTRA=(-DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake"
            -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26
            -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON)
        ;;
    *) echo 'usage: router/scripts/build.sh host|android' >&2; exit 2 ;;
esac
cmake -S "$ROOT/router" -B "$OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release "${EXTRA[@]}" "${@:2}"
cmake --build "$OUT" --target dejavu_router
