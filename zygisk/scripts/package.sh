#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
PROJECT_DIR=$(cd -- "${ROOT_DIR}/.." && pwd)
ANDROID_API=${ANDROID_API:-35}
MODULE_LIB=${MODULE_LIB:-"${ROOT_DIR}/out/lsplant-android-arm64-v8a-api${ANDROID_API}/libdejavu_zygisk.so"}
AGENT_LIB=${AGENT_LIB:-"${ROOT_DIR}/out/lsplant-android-arm64-v8a-api${ANDROID_API}/libdejavu_agent.so"}
DEJAVU_LIB=${DEJAVU_LIB:-"${PROJECT_DIR}/out/android-arm64-v8a-api${ANDROID_API}/libdejavu.so"}
TARGETS_FILE=${TARGETS_FILE:-"${ROOT_DIR}/config/targets.txt"}
INIT_LUA=${INIT_LUA:-"${ROOT_DIR}/config/init.lua"}
STAGE_DIR="${ROOT_DIR}/out/package"
DIST_DIR="${ROOT_DIR}/dist"
ZIP_PATH=${ZIP_PATH:-"${DIST_DIR}/dejavu-zygisk-v0.1.0-arm64.zip"}

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

rm -rf "${STAGE_DIR}"
mkdir -p \
    "${STAGE_DIR}/zygisk" \
    "${STAGE_DIR}/lib/arm64-v8a" \
    "${STAGE_DIR}/config" \
    "${DIST_DIR}"

cp "${ROOT_DIR}/module/module.prop" "${STAGE_DIR}/module.prop"
cp "${ROOT_DIR}/module/customize.sh" "${STAGE_DIR}/customize.sh"
cp "${ROOT_DIR}/module/skip_mount" "${STAGE_DIR}/skip_mount"
cp "${TARGETS_FILE}" "${STAGE_DIR}/config/targets.txt"
cp "${INIT_LUA}" "${STAGE_DIR}/config/init.lua"
cp "${MODULE_LIB}" "${STAGE_DIR}/zygisk/arm64-v8a.so"
cp "${AGENT_LIB}" "${STAGE_DIR}/lib/arm64-v8a/libdejavu_agent.so"
cp "${DEJAVU_LIB}" "${STAGE_DIR}/lib/arm64-v8a/libdejavu.so"

rm -f "${ZIP_PATH}"
(
    cd "${STAGE_DIR}"
    zip -qr "${ZIP_PATH}" .
)

echo "OK: ${ZIP_PATH}"
unzip -l "${ZIP_PATH}"
