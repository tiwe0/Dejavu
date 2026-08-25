#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
ANDROID_API=${ANDROID_API:-35}
BUILD_TYPE=${BUILD_TYPE:-release}
OUT_DIR=${OUT_DIR:-"${ROOT_DIR}/out/android-arm64-v8a-api${ANDROID_API}"}

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

HOST_TAG=linux-x86_64
case "$(uname -s)-$(uname -m)" in
    Darwin-*) HOST_TAG=darwin-x86_64 ;;
    Linux-x86_64) HOST_TAG=linux-x86_64 ;;
    *)
        echo "error: unsupported NDK host $(uname -s)-$(uname -m)" >&2
        exit 1
        ;;
esac

TOOLCHAIN="${NDK_DIR}/toolchains/llvm/prebuilt/${HOST_TAG}"
CC="${TOOLCHAIN}/bin/aarch64-linux-android${ANDROID_API}-clang"
READELF="${TOOLCHAIN}/bin/llvm-readelf"
NM="${TOOLCHAIN}/bin/llvm-nm"

if [[ ! -x "${CC}" ]]; then
    max_api=$(find "${TOOLCHAIN}/bin" -maxdepth 1 -name 'aarch64-linux-android*-clang' \
        -printf '%f\n' 2>/dev/null | sed -E 's/^.*android([0-9]+)-clang$/\1/' | sort -n | tail -n 1)
    echo "error: NDK ${NDK_DIR} does not provide Android API ${ANDROID_API} (max: ${max_api:-unknown})" >&2
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
GEN_DIR="${OUT_DIR}/generated"
LIB_PATH="${OUT_DIR}/libdejavu.so"
mkdir -p "${OBJ_DIR}" "${GEN_DIR}"

cat > "${GEN_DIR}/config.h" <<'EOF'
#ifndef TCC_CONFIG_H
#define TCC_CONFIG_H
#define TCC_VERSION "0.9.27-frida"
#define CONFIG_TCCDIR "/data/local/tmp/dejavu/tcc"
#endif
EOF

COMMON_FLAGS=(
    -std=c11
    -fPIC
    -ffunction-sections
    -fdata-sections
    -fvisibility=hidden
    -fno-omit-frame-pointer
    -D_FORTIFY_SOURCE=2
    "${OPT_FLAGS[@]}"
)

echo "[1/6] Dejavu API"
"${CC}" "${COMMON_FLAGS[@]}" -Wall -Wextra -Werror \
    -DDEJAVU_TCC_VERSION='"0.9.27-frida"' \
    -I"${ROOT_DIR}/include" \
    -I"${ROOT_DIR}/extern/lua" \
    -I"${ROOT_DIR}/extern/tinycc" \
    -c "${ROOT_DIR}/src/dejavu.c" -o "${OBJ_DIR}/dejavu.o"

echo "[2/6] Dejavu engine"
"${CC}" "${COMMON_FLAGS[@]}" -Wall -Wextra -Werror \
    -I"${ROOT_DIR}/include" \
    -I"${ROOT_DIR}/extern/tinycc" \
    -c "${ROOT_DIR}/src/dejavu_engine.c" -o "${OBJ_DIR}/dejavu_engine.o"

echo "[3/6] Dejavu Lua control"
"${CC}" "${COMMON_FLAGS[@]}" -Wall -Wextra -Werror \
    -I"${ROOT_DIR}/include" \
    -I"${ROOT_DIR}/extern/lua" \
    -c "${ROOT_DIR}/src/dejavu_lua.c" -o "${OBJ_DIR}/dejavu_lua.o"

echo "[4/6] Lua 5.4"
"${CC}" "${COMMON_FLAGS[@]}" \
    -DMAKE_LIB -DLUA_USE_LINUX \
    -I"${ROOT_DIR}/extern/lua" \
    -c "${ROOT_DIR}/extern/lua/onelua.c" -o "${OBJ_DIR}/lua.o"

echo "[5/6] TinyCC (Frida)"
"${CC}" "${COMMON_FLAGS[@]}" \
    -DONE_SOURCE=1 \
    -I"${GEN_DIR}" \
    -I"${ROOT_DIR}/extern/tinycc" \
    -c "${ROOT_DIR}/extern/tinycc/libtcc.c" -o "${OBJ_DIR}/tinycc.o"

echo "[6/6] libdejavu.so"
"${CC}" -shared \
    -Wl,-soname,libdejavu.so \
    -Wl,--gc-sections \
    -Wl,--no-undefined \
    -Wl,-z,max-page-size=16384 \
    -Wl,--version-script,"${ROOT_DIR}/config/dejavu.map" \
    "${OBJ_DIR}/dejavu.o" \
    "${OBJ_DIR}/dejavu_engine.o" \
    "${OBJ_DIR}/dejavu_lua.o" \
    "${OBJ_DIR}/lua.o" \
    "${OBJ_DIR}/tinycc.o" \
    -ldl -lm -pthread \
    -o "${LIB_PATH}"

echo "Verifying ELF..."
"${READELF}" -h "${LIB_PATH}" > "${OUT_DIR}/elf-header.txt"
"${READELF}" -d "${LIB_PATH}" > "${OUT_DIR}/elf-dynamic.txt"
"${READELF}" -lW "${LIB_PATH}" > "${OUT_DIR}/elf-program-headers.txt"
grep -q 'Class:.*ELF64' "${OUT_DIR}/elf-header.txt"
grep -q 'Machine:.*AArch64' "${OUT_DIR}/elf-header.txt"
grep -q 'Library soname: \[libdejavu.so\]' "${OUT_DIR}/elf-dynamic.txt"
load_alignments=$(awk '$1 == "LOAD" { print $NF }' "${OUT_DIR}/elf-program-headers.txt" | sort -u)
if [[ "${load_alignments}" != "0x4000" ]]; then
    echo "error: libdejavu.so PT_LOAD alignment is not 16 KB" >&2
    printf '%s\n' "${load_alignments}" >&2
    exit 1
fi

exports=$("${NM}" -D --defined-only "${LIB_PATH}" | awk '{ print $3 }' | sed 's/@.*//' | sort -u)
expected_exports=$(printf '%s\n' \
    dejavu_version \
    dejavu_lua_version \
    dejavu_tcc_version \
    dejavu_engine_create \
    dejavu_engine_destroy \
    dejavu_engine_add_symbol \
    dejavu_engine_compile \
    dejavu_module_symbol \
    dejavu_module_call \
    dejavu_module_destroy \
    dejavu_control_session_create \
    dejavu_control_session_destroy \
    dejavu_control_session_register \
    dejavu_control_session_eval \
    dejavu_control_session_invoke \
    dejavu_control_session_release_function \
    dejavu_value_release \
    dejavu_control_run \
    dejavu_smoke_lua \
    dejavu_smoke_tcc \
    dejavu_smoke | sort)
if [[ "${exports}" != "${expected_exports}" ]]; then
    echo "error: public symbol set does not match the Dejavu ABI" >&2
    diff -u <(printf '%s\n' "${expected_exports}") <(printf '%s\n' "${exports}") >&2 || true
    exit 1
fi

echo "OK: ${LIB_PATH}"
grep 'NEEDED' "${OUT_DIR}/elf-dynamic.txt" || true
