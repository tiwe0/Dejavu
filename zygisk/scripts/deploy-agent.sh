#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
ANDROID_API=${ANDROID_API:-35}
ADB_SERIAL=${ADB_SERIAL:-}
PROCESS=${PROCESS:-bin.mt.plus}
ACTIVITY=${ACTIVITY:-bin.mt.plus/.MainLightIcon}
AGENT_LIB=${AGENT_LIB:-"${ROOT_DIR}/out/lsplant-android-arm64-v8a-api${ANDROID_API}/libdejavu_agent.so"}
INIT_LUA=${INIT_LUA:-"${ROOT_DIR}/config/init.lua"}
HOOKX_LUA=${HOOKX_LUA:-"${ROOT_DIR}/config/hookx.lua"}

shell_quote() {
    printf "'%s'" "${1//\'/\'\\\'\'}"
}

adb_command=(adb)
if [[ -n "${ADB_SERIAL}" ]]; then
    adb_command+=(-s "${ADB_SERIAL}")
fi

if [[ ! -f "${AGENT_LIB}" ]]; then
    echo "error: Agent library not found: ${AGENT_LIB}" >&2
    exit 1
fi
if [[ ! -f "${INIT_LUA}" ]]; then
    echo "error: init script not found: ${INIT_LUA}" >&2
    exit 1
fi
if [[ ! -f "${HOOKX_LUA}" ]]; then
    echo "error: hookx script not found: ${HOOKX_LUA}" >&2
    exit 1
fi

rendered_init=$(mktemp /tmp/dejavu-init.XXXXXX.lua)
trap 'rm -f "${rendered_init}"' EXIT
{
    cat "${HOOKX_LUA}"
    printf '\n'
    cat "${INIT_LUA}"
} > "${rendered_init}"

remote_tmp="/data/local/tmp/libdejavu_agent.so.$$"
remote_init_tmp="/data/local/tmp/dejavu-init.lua.$$"
remote_tmp_q=$(shell_quote "${remote_tmp}")
remote_init_tmp_q=$(shell_quote "${remote_init_tmp}")
"${adb_command[@]}" push "${AGENT_LIB}" "${remote_tmp}"
"${adb_command[@]}" push "${rendered_init}" "${remote_init_tmp}"
"${adb_command[@]}" shell "su -c \"cp ${remote_tmp_q} /data/adb/modules/dejavu_zygisk/lib/arm64-v8a/libdejavu_agent.so.new && chmod 0755 /data/adb/modules/dejavu_zygisk/lib/arm64-v8a/libdejavu_agent.so.new && chcon u:object_r:system_file:s0 /data/adb/modules/dejavu_zygisk/lib/arm64-v8a/libdejavu_agent.so.new && mv -f /data/adb/modules/dejavu_zygisk/lib/arm64-v8a/libdejavu_agent.so.new /data/adb/modules/dejavu_zygisk/lib/arm64-v8a/libdejavu_agent.so && cp ${remote_init_tmp_q} /data/adb/modules/dejavu_zygisk/config/init.lua.new && mv -f /data/adb/modules/dejavu_zygisk/config/init.lua.new /data/adb/modules/dejavu_zygisk/config/init.lua && rm -f ${remote_tmp_q} ${remote_init_tmp_q}\""
"${adb_command[@]}" shell "am force-stop '${PROCESS}'; sleep 1; am start -W -n '${ACTIVITY}'"
