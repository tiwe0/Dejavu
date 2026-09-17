#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
PROJECT_DIR=$(cd -- "${ROOT_DIR}/.." && pwd)
ANDROID_API=${ANDROID_API:-26}
BUILD_TYPE=${BUILD_TYPE:-release}
OUT_DIR=${OUT_DIR:-"${ROOT_DIR}/out/android-arm64-v8a-api${ANDROID_API}"}
LSPLANT_BACKEND_ARCHIVE=${LSPLANT_BACKEND_ARCHIVE:-}
RUN_DEVICE_SMOKE=${RUN_DEVICE_SMOKE:-0}

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

case "$(uname -s)-$(uname -m)" in
    Darwin-*) HOST_TAG=darwin-x86_64 ;;
    Linux-x86_64) HOST_TAG=linux-x86_64 ;;
    *)
        echo "error: unsupported NDK host $(uname -s)-$(uname -m)" >&2
        exit 1
        ;;
esac

TOOLCHAIN="${NDK_DIR}/toolchains/llvm/prebuilt/${HOST_TAG}"
CXX="${TOOLCHAIN}/bin/aarch64-linux-android${ANDROID_API}-clang++"
READELF="${TOOLCHAIN}/bin/llvm-readelf"
NM="${TOOLCHAIN}/bin/llvm-nm"

if [[ ! -x "${CXX}" ]]; then
    echo "error: NDK does not provide the Android API ${ANDROID_API} arm64 compiler" >&2
    exit 1
fi

case "${BUILD_TYPE}" in
    debug) OPT_FLAGS=(-O0 -g3) ;;
    release) OPT_FLAGS=(-O2 -g0) ;;
    *)
        echo "error: BUILD_TYPE must be 'debug' or 'release'" >&2
        exit 1
        ;;
esac

OBJ_DIR="${OUT_DIR}/obj"
LIB_PATH="${OUT_DIR}/libdejavu_zygisk.so"
mkdir -p "${OBJ_DIR}"

COMMON_FLAGS=(
    -std=c++17
    -fPIC
    -fno-exceptions
    -fno-rtti
    -ffunction-sections
    -fdata-sections
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -fno-omit-frame-pointer
    -D_FORTIFY_SOURCE=2
    -Wall
    -Wextra
    -Werror
    "${OPT_FLAGS[@]}"
    -I"${ROOT_DIR}/include"
    -I"${PROJECT_DIR}/include"
)

if [[ -n "${LSPLANT_BACKEND_ARCHIVE}" ]]; then
    if [[ ! -f "${LSPLANT_BACKEND_ARCHIVE}" ]]; then
        echo "error: LSPLANT_BACKEND_ARCHIVE does not exist: ${LSPLANT_BACKEND_ARCHIVE}" >&2
        exit 1
    fi
    COMMON_FLAGS+=(-DDEJAVU_WITH_LSPLANT=1)
else
    COMMON_FLAGS+=(-DDEJAVU_WITH_LSPLANT=0)
fi
case "${RUN_DEVICE_SMOKE}" in
    0|1) COMMON_FLAGS+=(-DDEJAVU_RUN_DEVICE_SMOKE="${RUN_DEVICE_SMOKE}") ;;
    *)
        echo "error: RUN_DEVICE_SMOKE must be 0 or 1" >&2
        exit 1
        ;;
esac

for source in module dejavu_runtime lsplant_runtime target_config; do
    "${CXX}" "${COMMON_FLAGS[@]}" \
        -c "${ROOT_DIR}/src/${source}.cpp" -o "${OBJ_DIR}/${source}.o"
done

LINK_INPUTS=(
    "${OBJ_DIR}/module.o"
    "${OBJ_DIR}/dejavu_runtime.o"
    "${OBJ_DIR}/lsplant_runtime.o"
    "${OBJ_DIR}/target_config.o"
)
if [[ -n "${LSPLANT_BACKEND_ARCHIVE}" ]]; then
    LINK_INPUTS+=(
        -Wl,--whole-archive
        "${LSPLANT_BACKEND_ARCHIVE}"
        -Wl,--no-whole-archive
    )
fi

"${CXX}" -shared -nostdlib++ \
    -Wl,-soname,libdejavu_zygisk.so \
    -Wl,--gc-sections \
    -Wl,--no-undefined \
    -Wl,-z,max-page-size=16384 \
    -Wl,--version-script,"${ROOT_DIR}/config/exports.map" \
    "${LINK_INPUTS[@]}" \
    -lstdc++ -llog -ldl \
    -o "${LIB_PATH}"

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
if [[ "${exports}" != "zygisk_module_entry" ]]; then
    echo "error: unexpected Zygisk export set" >&2
    printf '%s\n' "${exports}" >&2
    exit 1
fi
if grep -qE 'Shared library: \[(libdejavu|libc\+\+_shared)\.so\]' "${OUT_DIR}/elf-dynamic.txt"; then
    echo "error: Zygisk entry library has a forbidden private DT_NEEDED dependency" >&2
    exit 1
fi

echo "OK: ${LIB_PATH}"
grep 'NEEDED' "${OUT_DIR}/elf-dynamic.txt" || true
