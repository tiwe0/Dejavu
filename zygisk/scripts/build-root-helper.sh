#!/usr/bin/env bash
# Build the prototype root helper for Android arm64.
set -euo pipefail
ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ -z "${ANDROID_NDK_HOME:-}" && -f "${ROOT_DIR}/../out/tooling/env.sh" ]]; then
    # shellcheck disable=SC1091
    source "${ROOT_DIR}/../out/tooling/env.sh"
fi
if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
    echo "error: ANDROID_NDK_HOME is not set" >&2
    exit 1
fi
CLANGXX="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang++"
OUT_DIR="${ROOT_DIR}/out/root-helper"
mkdir -p "${OUT_DIR}"
"${CLANGXX}" -std=c++17 -O2 -Wall -Wextra -fPIE -pie \
    "${ROOT_DIR}/root_helper/root_helper.cpp" \
    -static-libstdc++ -o "${OUT_DIR}/dejavu-root-helper"
echo "OK: ${OUT_DIR}/dejavu-root-helper"
