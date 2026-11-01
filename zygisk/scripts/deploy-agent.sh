#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
PROJECT_DIR=$(cd -- "${ROOT_DIR}/.." && pwd)
ANDROID_API=${ANDROID_API:-26}
ADB=${ADB:-adb}
ADB_SERIAL=${ADB_SERIAL:-}
NO_RESTART=${NO_RESTART:-0}
PROCESS=${PROCESS:-bin.mt.plus}
ACTIVITY=${ACTIVITY:-bin.mt.plus/.MainLightIcon}
AGENT_LIB=${AGENT_LIB:-"${ROOT_DIR}/out/lsplant-android-arm64-v8a-api${ANDROID_API}/libdejavu_agent.so"}
CORE_LIB=${CORE_LIB:-"${PROJECT_DIR}/out/android-arm64-v8a-api${ANDROID_API}/libdejavu.so"}
INIT_LUA=${INIT_LUA:-"${ROOT_DIR}/config/init.lua"}
HOOKX_LUA=${HOOKX_LUA:-"${ROOT_DIR}/config/hookx.lua"}

case "${PROCESS}" in
    ''|*[!A-Za-z0-9_.:-]*)
        echo "error: PROCESS must match [A-Za-z0-9_.:-]+" >&2
        exit 1
        ;;
esac
case "${ACTIVITY}" in
    ''|*[!A-Za-z0-9_./:-]*)
        echo "error: ACTIVITY must match [A-Za-z0-9_./:-]+" >&2
        exit 1
        ;;
esac

adb_command=("${ADB}")
if [[ -n "${ADB_SERIAL}" ]]; then
    adb_command+=(-s "${ADB_SERIAL}")
fi

if [[ ! -f "${AGENT_LIB}" ]]; then
    echo "error: Agent library not found: ${AGENT_LIB}" >&2
    exit 1
fi
if [[ ! -f "${CORE_LIB}" ]]; then
    echo "error: core library not found: ${CORE_LIB}" >&2
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

mkdir -p "${PROJECT_DIR}/out/tooling"
rendered_init=$(mktemp "${PROJECT_DIR}/out/tooling/dejavu-init.XXXXXX.lua")
trap 'rm -f "${rendered_init}"' EXIT
{
    cat "${HOOKX_LUA}"
    printf '\n'
    cat "${INIT_LUA}"
} > "${rendered_init}"

remote_suffix=$$
case "${remote_suffix}" in
    ''|*[!0-9]*)
        echo "error: invalid deploy-agent suffix: ${remote_suffix}" >&2
        exit 1
        ;;
esac
remote_tmp="/data/local/tmp/libdejavu_agent.so.${remote_suffix}"
remote_init_tmp="/data/local/tmp/dejavu-init.lua.${remote_suffix}"
remote_core_tmp="/data/local/tmp/libdejavu.so.${remote_suffix}"
deploy_script="/data/local/tmp/dejavu-deploy.${remote_suffix}.sh"
"${adb_command[@]}" push "${AGENT_LIB}" "${remote_tmp}" >/dev/null
"${adb_command[@]}" push "${CORE_LIB}" "${remote_core_tmp}" >/dev/null
"${adb_command[@]}" push "${rendered_init}" "${remote_init_tmp}" >/dev/null

local_script=$(mktemp "${PROJECT_DIR}/out/tooling/dejavu-deploy.XXXXXX.sh")
cleanup() {
    rm -f "${local_script}" "${rendered_init}"
}
trap cleanup EXIT
cat > "${local_script}" <<'DEPLOY_SCRIPT'
set -e
module=/data/adb/modules/dejavu_zygisk
suffix=__SUFFIX__
staging="${module}/.deploy.${suffix}"
backup="${module}/.deploy-backup.${suffix}"
mkdir -p "${staging}/lib/arm64-v8a" "${staging}/config"
cp "/data/local/tmp/libdejavu.so.${suffix}" "${staging}/lib/arm64-v8a/libdejavu.so"
cp "/data/local/tmp/libdejavu_agent.so.${suffix}" "${staging}/lib/arm64-v8a/libdejavu_agent.so"
cp "/data/local/tmp/dejavu-init.lua.${suffix}" "${staging}/config/init.lua"
chmod 0755 "${staging}/lib/arm64-v8a/libdejavu.so" "${staging}/lib/arm64-v8a/libdejavu_agent.so"
chmod 0644 "${staging}/config/init.lua"
chcon u:object_r:system_file:s0 "${staging}/lib/arm64-v8a/libdejavu.so"     "${staging}/lib/arm64-v8a/libdejavu_agent.so" "${staging}/config/init.lua"
mkdir -p "${backup}"
for target in     "${module}/lib/arm64-v8a/libdejavu.so"     "${module}/lib/arm64-v8a/libdejavu_agent.so"     "${module}/config/init.lua"; do
    if [ -f "${target}" ]; then
        cp "${target}" "${backup}/$(basename "${target}")"
    fi
done
restore() {
    status=$?
    for target in         "${module}/lib/arm64-v8a/libdejavu.so"         "${module}/lib/arm64-v8a/libdejavu_agent.so"         "${module}/config/init.lua"; do
        saved="${backup}/$(basename "${target}")"
        if [ -f "${saved}" ]; then
            cp "${saved}" "${target}"
        else
            rm -f "${target}"
        fi
    done
    rm -rf "${staging}" "${backup}"
    return "${status}"
}
trap restore EXIT
cp "${staging}/lib/arm64-v8a/libdejavu.so" "${module}/lib/arm64-v8a/libdejavu.so"
cp "${staging}/lib/arm64-v8a/libdejavu_agent.so" "${module}/lib/arm64-v8a/libdejavu_agent.so"
cp "${staging}/config/init.lua" "${module}/config/init.lua"
trap - EXIT
rm -rf "${staging}" "${backup}"
rm -f "/data/local/tmp/libdejavu.so.${suffix}"     "/data/local/tmp/libdejavu_agent.so.${suffix}"     "/data/local/tmp/dejavu-init.lua.${suffix}" "/data/local/tmp/dejavu-deploy.${suffix}.sh"
DEPLOY_SCRIPT
sed -i "s/__SUFFIX__/${remote_suffix}/g" "${local_script}"
"${adb_command[@]}" push "${local_script}" "${deploy_script}" >/dev/null
"${adb_command[@]}" shell "su -c 'sh ${deploy_script}'"

if [[ "${NO_RESTART}" != "1" ]]; then
    "${adb_command[@]}" shell "am force-stop '${PROCESS}'; sleep 1; am start -W -n '${ACTIVITY}'"
fi
