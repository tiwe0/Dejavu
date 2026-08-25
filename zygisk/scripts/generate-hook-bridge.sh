#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
OUT_DIR=${1:?usage: generate-hook-bridge.sh OUT_DIR}
ANDROID_HOME=${ANDROID_HOME:-"${HOME}/Library/Android/sdk"}
RUN_DEVICE_STRESS=${DEJAVU_RUN_DEVICE_STRESS:-0}

find_d8() {
    find "${ANDROID_HOME}/build-tools" -mindepth 2 -maxdepth 2 -type f -name d8 -print 2>/dev/null |
        sort -V | tail -n 1
}

D8=$(find_d8)
if [[ -z "${D8}" ]]; then
    echo "error: Android d8 not found under ${ANDROID_HOME}/build-tools" >&2
    exit 1
fi
if [[ "$(uname -s)" == "Darwin" ]] && /usr/libexec/java_home -v 17 >/dev/null 2>&1; then
    export JAVA_HOME=$(/usr/libexec/java_home -v 17)
fi

CLASSES_DIR="${OUT_DIR}/classes"
DEX_DIR="${OUT_DIR}/dex"
rm -rf "${CLASSES_DIR}" "${DEX_DIR}"
mkdir -p "${CLASSES_DIR}" "${DEX_DIR}"
sources=("${ROOT_DIR}/java/io/dejavu/bridge/HookBridge.java")
if [[ "${RUN_DEVICE_STRESS}" == "1" ]]; then
    sources+=("${ROOT_DIR}/tests/device/java/io/dejavu/bridge/HookStress.java")
elif [[ "${RUN_DEVICE_STRESS}" != "0" ]]; then
    echo "error: DEJAVU_RUN_DEVICE_STRESS must be 0 or 1" >&2
    exit 1
fi
javac --release 8 \
    -d "${CLASSES_DIR}" \
    "${sources[@]}"
jar --create --file "${OUT_DIR}/hook-bridge.jar" -C "${CLASSES_DIR}" .
"${D8}" --min-api 26 --output "${DEX_DIR}" "${OUT_DIR}/hook-bridge.jar"
xxd -i -n dejavu_hook_bridge_dex "${DEX_DIR}/classes.dex" > "${OUT_DIR}/hook_bridge_dex.h"
