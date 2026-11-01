#!/usr/bin/env bash
# Build the SSL plaintext capture example plugin.
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
if [[ -z "${ANDROID_NDK_HOME:-}" && -f "${ROOT_DIR}/out/tooling/env.sh" ]]; then
    # shellcheck disable=SC1091
    source "${ROOT_DIR}/out/tooling/env.sh"
fi
if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
    echo "error: ANDROID_NDK_HOME is not set" >&2
    exit 1
fi
OUT_DIR=${OUT_DIR:-"${ROOT_DIR}/out/plugins/capture"}
cmake -S "${ROOT_DIR}/plugins/capture" -B "${OUT_DIR}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-26 \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "${OUT_DIR}"
echo "OK: ${OUT_DIR}/libdejavu_capture.so"
