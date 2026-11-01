#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
PROJECT_DIR=$(cd -- "${ROOT_DIR}/.." && pwd)
ANDROID_API=${ANDROID_API:-26}
MODULE_LIB=${MODULE_LIB:-"${ROOT_DIR}/out/lsplant-android-arm64-v8a-api${ANDROID_API}/libdejavu_zygisk.so"}
AGENT_LIB=${AGENT_LIB:-"${ROOT_DIR}/out/lsplant-android-arm64-v8a-api${ANDROID_API}/libdejavu_agent.so"}
DEJAVU_LIB=${DEJAVU_LIB:-"${PROJECT_DIR}/out/android-arm64-v8a-api${ANDROID_API}/libdejavu.so"}
TARGETS_FILE=${TARGETS_FILE:-"${ROOT_DIR}/config/targets.txt"}
INIT_LUA=${INIT_LUA:-"${ROOT_DIR}/config/init.lua"}
HOOKX_LUA=${HOOKX_LUA:-"${ROOT_DIR}/config/hookx.lua"}
WEBROOT_DIR=${WEBROOT_DIR:-"${ROOT_DIR}/webroot"}
LICENSE_FILE=${LICENSE_FILE:-"${PROJECT_DIR}/LICENSE"}
NOTICES_FILE=${NOTICES_FILE:-"${PROJECT_DIR}/THIRD_PARTY_NOTICES.md"}
STAGE_DIR="${ROOT_DIR}/out/package"
DIST_DIR="${ROOT_DIR}/dist"
if [[ -z "${PACKAGE_VERSION:-}" ]]; then
    PACKAGE_VERSION=$(sed -n 's/^version=v//p' "${ROOT_DIR}/module/module.prop" | head -n 1)
fi
PACKAGE_VERSION=${PACKAGE_VERSION:-0.5.0}
ZIP_PATH=${ZIP_PATH:-"${DIST_DIR}/dejavu-zygisk-v${PACKAGE_VERSION}-arm64.zip"}
LLVM_STRIP=${LLVM_STRIP:-llvm-strip}

if [[ ! -f "${MODULE_LIB}" ]]; then
    echo "error: Zygisk module library not found: ${MODULE_LIB}" >&2
    exit 1
fi
if [[ ! -f "${AGENT_LIB}" ]]; then
    echo "error: Dejavu Agent library not found: ${AGENT_LIB}" >&2
    exit 1
fi
if [[ ! -f "${DEJAVU_LIB}" ]]; then
    echo "error: Dejavu library not found: ${DEJAVU_LIB}" >&2
    echo "build it first with: make -C ${PROJECT_DIR} android-arm64" >&2
    exit 1
fi
if [[ ! -f "${TARGETS_FILE}" ]]; then
    echo "error: target process file not found: ${TARGETS_FILE}" >&2
    exit 1
fi
if [[ ! -f "${INIT_LUA}" ]]; then
    echo "error: init Lua file not found: ${INIT_LUA}" >&2
    exit 1
fi
if [[ ! -f "${HOOKX_LUA}" ]]; then
    echo "error: hookx Lua file not found: ${HOOKX_LUA}" >&2
    exit 1
fi
if [[ ! -f "${LICENSE_FILE}" ]]; then
    echo "error: project license file not found: ${LICENSE_FILE}" >&2
    exit 1
fi
if [[ ! -f "${NOTICES_FILE}" ]]; then
    echo "error: third-party notices file not found: ${NOTICES_FILE}" >&2
    exit 1
fi
if [[ ! -f "${WEBROOT_DIR}/index.html" ]]; then
    echo "error: WebUI entrypoint not found: ${WEBROOT_DIR}/index.html" >&2
    exit 1
fi

resolve_llvm_strip() {
    if [[ "${LLVM_STRIP}" == */* ]]; then
        [[ -x "${LLVM_STRIP}" ]] && return 0
    elif command -v "${LLVM_STRIP}" >/dev/null 2>&1; then
        LLVM_STRIP=$(command -v "${LLVM_STRIP}")
        return 0
    fi

    local root candidate
    for root in \
        "${ANDROID_NDK_HOME:-}" \
        "${ANDROID_NDK_ROOT:-}" \
        "${HOME}/Library/Android/sdk/ndk" \
        "${HOME}/Android/Sdk/ndk"; do
        [[ -d "${root}" ]] || continue
        candidate=$(find "${root}" \
            -path '*/toolchains/llvm/prebuilt/*/bin/llvm-strip' \
            -print -quit 2>/dev/null || true)
        if [[ -n "${candidate}" && -x "${candidate}" ]]; then
            LLVM_STRIP=${candidate}
            return 0
        fi
    done
    return 1
}

if ! resolve_llvm_strip; then
    echo "error: llvm-strip not found; set LLVM_STRIP to the Android NDK llvm-strip" >&2
    exit 1
fi

rm -rf "${STAGE_DIR}"
mkdir -p \
    "${STAGE_DIR}/zygisk" \
    "${STAGE_DIR}/lib/arm64-v8a" \
    "${STAGE_DIR}/config" \
    "${STAGE_DIR}/webroot" \
    "${DIST_DIR}"

cp "${ROOT_DIR}/module/module.prop" "${STAGE_DIR}/module.prop"
cp "${ROOT_DIR}/module/customize.sh" "${STAGE_DIR}/customize.sh"
cp "${ROOT_DIR}/module/skip_mount" "${STAGE_DIR}/skip_mount"
cp "${TARGETS_FILE}" "${STAGE_DIR}/config/targets.txt"
{
    cat "${HOOKX_LUA}"
    printf '\n'
    cat "${INIT_LUA}"
} > "${STAGE_DIR}/config/init.lua"
cp -R "${WEBROOT_DIR}/." "${STAGE_DIR}/webroot/"
cp "${MODULE_LIB}" "${STAGE_DIR}/zygisk/arm64-v8a.so"
cp "${AGENT_LIB}" "${STAGE_DIR}/lib/arm64-v8a/libdejavu_agent.so"
cp "${DEJAVU_LIB}" "${STAGE_DIR}/lib/arm64-v8a/libdejavu.so"
cp "${LICENSE_FILE}" "${STAGE_DIR}/LICENSE"
cp "${NOTICES_FILE}" "${STAGE_DIR}/THIRD_PARTY_NOTICES.md"
mkdir -p "${STAGE_DIR}/LICENSES"

copy_license() {
    local source=$1
    local name=$2
    if [[ -f "${source}" ]]; then
        cp "${source}" "${STAGE_DIR}/LICENSES/${name}"
    else
        echo "warning: license text not found: ${source}" >&2
    fi
}
copy_license "${PROJECT_DIR}/extern/tinycc/COPYING" "TinyCC-LGPL-2.1.txt"
copy_license "${ROOT_DIR}/third_party/LSPlant/LICENSE" "LSPlant-LGPL-3.0.txt"
copy_license \
    "${ROOT_DIR}/third_party/LSPlant/lsplant/src/main/jni/external/dex_builder/LICENSE" \
    "DexBuilder-LGPL-3.0.txt"
copy_license \
    "${ROOT_DIR}/third_party/LSPlant/lsplant/src/main/jni/external/dex_builder/external/parallel_hashmap/LICENSE" \
    "parallel-hashmap-Apache-2.0.txt"
copy_license "${ROOT_DIR}/third_party/Dobby/LICENSE" "Dobby-Apache-2.0.txt"
if [[ -f "${PROJECT_DIR}/extern/lua/lua.h" ]]; then
    {
        printf 'Lua 5.4.8 - MIT License\n\n'
        sed -n '/Copyright (C) 1994-2025 Lua.org/,$p' \
            "${PROJECT_DIR}/extern/lua/lua.h" | sed -n '1,24p'
    } > "${STAGE_DIR}/LICENSES/Lua-MIT.txt"
fi
if [[ -f "${ROOT_DIR}/include/zygisk.hpp" ]]; then
    {
        printf 'Zygisk API header - 0BSD License\n\n'
        sed -n '1,13p' "${ROOT_DIR}/include/zygisk.hpp"
    } > "${STAGE_DIR}/LICENSES/Zygisk-0BSD.txt"
fi

# Keep the unstripped build outputs in out/ for symbolized debugging. The
# installable module contains only runtime sections.
"${LLVM_STRIP}" --strip-debug \
    "${STAGE_DIR}/zygisk/arm64-v8a.so" \
    "${STAGE_DIR}/lib/arm64-v8a/libdejavu_agent.so" \
    "${STAGE_DIR}/lib/arm64-v8a/libdejavu.so"

rm -f "${ZIP_PATH}"
(
    cd "${STAGE_DIR}"
    zip -qr "${ZIP_PATH}" .
)

echo "OK: ${ZIP_PATH}"
unzip -l "${ZIP_PATH}"
