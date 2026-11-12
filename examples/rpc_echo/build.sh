#!/usr/bin/env bash
# Build the small ABI/contract plugin used by examples and CI.
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
OUT_DIR=${OUT_DIR:-"${ROOT_DIR}/out/plugins/rpc_echo"}
mkdir -p "${OUT_DIR}"
OUT_DIR=$(cd -- "${OUT_DIR}" && pwd)
BUILD_DIR="${OUT_DIR}/.examples-build"
cmake -S "${ROOT_DIR}/examples/rpc_echo" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_LIBRARY_OUTPUT_DIRECTORY="${OUT_DIR}" \
    -DCMAKE_TOOLCHAIN_FILE="${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-26 \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}"
cp "${ROOT_DIR}/examples/rpc_echo/manifest.json" "${ROOT_DIR}/examples/rpc_echo/SKILL.md" "${OUT_DIR}/"
echo "OK: ${OUT_DIR}/libdejavu_rpc_echo.so"
