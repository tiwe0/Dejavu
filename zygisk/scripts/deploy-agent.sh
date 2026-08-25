#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
ANDROID_API=${ANDROID_API:-35}
ADB_SERIAL=${ADB_SERIAL:-}
PROCESS=${PROCESS:-bin.mt.plus}
ACTIVITY=${ACTIVITY:-bin.mt.plus/.MainLightIcon}
AGENT_LIB=${AGENT_LIB:-"${ROOT_DIR}/out/lsplant-android-arm64-v8a-api${ANDROID_API}/libdejavu_agent.so"}

adb_command=(adb)
if [[ -n "${ADB_SERIAL}" ]]; then
    adb_command+=(-s "${ADB_SERIAL}")
fi

if [[ ! -f "${AGENT_LIB}" ]]; then
    echo "error: Agent library not found: ${AGENT_LIB}" >&2
    exit 1
fi

remote_tmp="/data/local/tmp/libdejavu_agent.so.$$"
"${adb_command[@]}" push "${AGENT_LIB}" "${remote_tmp}"
"${adb_command[@]}" shell "su -c 'cp ${remote_tmp} /data/adb/modules/dejavu_zygisk/lib/arm64-v8a/libdejavu_agent.so.new && chmod 0755 /data/adb/modules/dejavu_zygisk/lib/arm64-v8a/libdejavu_agent.so.new && chcon u:object_r:system_file:s0 /data/adb/modules/dejavu_zygisk/lib/arm64-v8a/libdejavu_agent.so.new && mv -f /data/adb/modules/dejavu_zygisk/lib/arm64-v8a/libdejavu_agent.so.new /data/adb/modules/dejavu_zygisk/lib/arm64-v8a/libdejavu_agent.so && rm -f ${remote_tmp}'"
"${adb_command[@]}" shell "am force-stop '${PROCESS}'; sleep 1; am start -W -n '${ACTIVITY}'"
