#!/usr/bin/env bash
# Cross-build dejavurouter for Android arm64.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NDK="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/android-ndk-r27d}"
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
CC="$TC/bin/aarch64-linux-android26-clang"
LIBEVENT_SRC="${LIBEVENT_SRC:-/tmp/libevent-src}"
LIBEVENT_PREFIX="${LIBEVENT_PREFIX:-/tmp/libevent-android}"

if [ ! -x "$CC" ]; then
    echo "Android NDK clang not found: $CC" >&2
    exit 1
fi

if [ ! -f "$LIBEVENT_PREFIX/include/event2/event.h" ]; then
    if [ ! -f "$LIBEVENT_SRC/configure" ]; then
        mkdir -p /tmp/dejavurouter-build
        curl -sSL -o /tmp/dejavurouter-build/libevent.tar.gz \
            https://github.com/libevent/libevent/releases/download/release-2.1.12-stable/libevent-2.1.12-stable.tar.gz
        rm -rf "$LIBEVENT_SRC"
        mkdir -p "$LIBEVENT_SRC"
        tar -xzf /tmp/dejavurouter-build/libevent.tar.gz -C "$LIBEVENT_SRC" --strip-components=1
    fi
    cd "$LIBEVENT_SRC"
    ./configure --host=aarch64-linux-android --disable-shared --enable-static \
        --disable-openssl --disable-samples \
        CC="$CC --sysroot=$TC/sysroot" \
        AR="$TC/bin/llvm-ar" RANLIB="$TC/bin/llvm-ranlib" \
        --prefix="$LIBEVENT_PREFIX" >/dev/null
    make -j"$(nproc)" >/dev/null
    make install >/dev/null
fi

cd "$ROOT"
"$CC" -O2 -g -std=c11 -Wall -Wextra -pthread \
    -Ithird_party/cJSON -I"$LIBEVENT_PREFIX/include" \
    -o dejavurouter-android src/async_router.c third_party/cJSON/cJSON.c \
    "$LIBEVENT_PREFIX/lib/libevent_extra.a" \
    "$LIBEVENT_PREFIX/lib/libevent_core.a" -lm
file dejavurouter-android
