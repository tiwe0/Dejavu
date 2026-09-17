#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
ANDROID_API=${ANDROID_API:-26}
BUILD_TYPE=${BUILD_TYPE:-Release}
RUN_DEVICE_SMOKE=${RUN_DEVICE_SMOKE:-0}
RUN_DEVICE_STRESS=${RUN_DEVICE_STRESS:-0}
OUT_DIR=${OUT_DIR:-"${ROOT_DIR}/out/lsplant-android-arm64-v8a-api${ANDROID_API}"}

find_ndk() {
    local candidate
    for candidate in \
        "${ANDROID_NDK_HOME:-}" \
        "${ANDROID_NDK_ROOT:-}" \
        "${ANDROID_HOME:-}/ndk" \
        "${HOME}/Library/Android/sdk/ndk" \
        "${HOME}/Android/Sdk/ndk"; do
        if [[ -f "${candidate}/source.properties" ]]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
        if [[ -d "${candidate}" ]]; then
            find "${candidate}" -mindepth 1 -maxdepth 1 -type d \
                -exec test -f '{}/source.properties' \; -print 2>/dev/null | sort -V | tail -n 1
            return 0
        fi
    done
    return 1
}

NDK_DIR=$(find_ndk || true)
if [[ -z "${NDK_DIR}" ]]; then
    echo "error: Android NDK not found; set ANDROID_NDK_HOME" >&2
    exit 1
fi
NDK_REVISION=$(sed -n 's/^Pkg\.Revision = //p' "${NDK_DIR}/source.properties" | head -n 1)
NDK_MAJOR=${NDK_REVISION%%.*}
if [[ ! "${NDK_MAJOR}" =~ ^[0-9]+$ ]] || ((NDK_MAJOR < 29)); then
    echo "error: LSPlant requires Android NDK r29 or newer (found ${NDK_REVISION:-unknown})" >&2
    exit 1
fi
case "${RUN_DEVICE_SMOKE}" in
    0|1) ;;
    *) echo "error: RUN_DEVICE_SMOKE must be 0 or 1" >&2; exit 1 ;;
esac
case "${RUN_DEVICE_STRESS}" in
    0|1) ;;
    *) echo "error: RUN_DEVICE_STRESS must be 0 or 1" >&2; exit 1 ;;
esac

"${ROOT_DIR}/scripts/fetch-third-party.sh"

TOOLCHAIN_FILE="${NDK_DIR}/build/cmake/android.toolchain.cmake"
if [[ -f "${OUT_DIR}/CMakeCache.txt" ]]; then
    CACHED_TOOLCHAIN=$(sed -n 's/^CMAKE_TOOLCHAIN_FILE:[^=]*=//p' \
        "${OUT_DIR}/CMakeCache.txt" | head -n 1)
    if [[ -n "${CACHED_TOOLCHAIN}" && "${CACHED_TOOLCHAIN}" != "${TOOLCHAIN_FILE}" ]]; then
        echo "Removing stale CMake cache for ${CACHED_TOOLCHAIN}"
        find "${OUT_DIR}" -mindepth 1 -delete
    fi
fi

cmake -S "${ROOT_DIR}" -B "${OUT_DIR}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM="android-${ANDROID_API}" \
    -DANDROID_STL=c++_static \
    -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DDEJAVU_RUN_DEVICE_SMOKE="${RUN_DEVICE_SMOKE}" \
    -DDEJAVU_RUN_DEVICE_STRESS="${RUN_DEVICE_STRESS}"
cmake --build "${OUT_DIR}" --target dejavu_zygisk dejavu_agent

TOOLCHAIN=$(find "${NDK_DIR}/toolchains/llvm/prebuilt" -mindepth 1 -maxdepth 1 -type d -print -quit)
READELF="${TOOLCHAIN}/bin/llvm-readelf"
NM="${TOOLCHAIN}/bin/llvm-nm"
LIB_PATH="${OUT_DIR}/libdejavu_zygisk.so"
AGENT_PATH="${OUT_DIR}/libdejavu_agent.so"

"${READELF}" -h "${LIB_PATH}" > "${OUT_DIR}/elf-header.txt"
"${READELF}" -d "${LIB_PATH}" > "${OUT_DIR}/elf-dynamic.txt"
"${READELF}" -lW "${LIB_PATH}" > "${OUT_DIR}/elf-program-headers.txt"
grep -q 'Class:.*ELF64' "${OUT_DIR}/elf-header.txt"
grep -q 'Machine:.*AArch64' "${OUT_DIR}/elf-header.txt"
grep -q 'Library soname: \[libdejavu_zygisk.so\]' "${OUT_DIR}/elf-dynamic.txt"

load_alignments=$(awk '$1 == "LOAD" { print $NF }' "${OUT_DIR}/elf-program-headers.txt" | sort -u)
if [[ "${load_alignments}" != "0x4000" ]]; then
    echo "error: Zygisk module PT_LOAD alignment is not 16 KB" >&2
    printf '%s\n' "${load_alignments}" >&2
    exit 1
fi
exports=$("${NM}" -D --defined-only "${LIB_PATH}" | awk '{print $3}' | sed 's/@.*//' | sort -u)
if [[ "${exports}" != $'zygisk_companion_entry\nzygisk_module_entry' ]]; then
    echo "error: unexpected Zygisk export set" >&2
    printf '%s\n' "${exports}" >&2
    exit 1
fi
if grep -qE 'Shared library: \[(libdejavu|libc\+\+_shared|libdobby|liblsplant)\.so\]' \
    "${OUT_DIR}/elf-dynamic.txt"; then
    echo "error: Zygisk entry library has a forbidden private DT_NEEDED dependency" >&2
    exit 1
fi

echo "OK: ${LIB_PATH}"
grep 'NEEDED' "${OUT_DIR}/elf-dynamic.txt" || true
test -f "${AGENT_PATH}"
"${READELF}" -h "${AGENT_PATH}" > "${OUT_DIR}/agent-elf-header.txt"
"${READELF}" -d "${AGENT_PATH}" > "${OUT_DIR}/agent-elf-dynamic.txt"
"${READELF}" -lW "${AGENT_PATH}" > "${OUT_DIR}/agent-elf-program-headers.txt"
grep -q 'Class:.*ELF64' "${OUT_DIR}/agent-elf-header.txt"
grep -q 'Machine:.*AArch64' "${OUT_DIR}/agent-elf-header.txt"
agent_alignments=$(awk '$1 == "LOAD" { print $NF }' "${OUT_DIR}/agent-elf-program-headers.txt" | sort -u)
if [[ "${agent_alignments}" != "0x4000" ]]; then
    echo "error: Agent PT_LOAD alignment is not 16 KB" >&2
    printf '%s\n' "${agent_alignments}" >&2
    exit 1
fi
agent_exports=$(${NM} -D --defined-only "${AGENT_PATH}" | awk '{print $3}' | sed 's/@.*//' | sort -u)
if [[ "${agent_exports}" != "dejavu_agent_get_api" ]]; then
    echo "error: unexpected Agent export set" >&2
    printf '%s\n' "${agent_exports}" >&2
    exit 1
fi
echo "OK: ${AGENT_PATH}"
