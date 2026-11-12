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
source "${ROOT_DIR}/scripts/strip-runtime.sh"

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
local_stage=$(mktemp -d "${PROJECT_DIR}/out/tooling/dejavu-deploy.XXXXXX")
trap 'rm -rf "${local_stage}"' EXIT
chmod 0700 "${local_stage}"
rendered_init="${local_stage}/init.lua"
local_script="${local_stage}/deploy.sh"
cp "${AGENT_LIB}" "${local_stage}/libdejavu_agent.so"
cp "${CORE_LIB}" "${local_stage}/libdejavu.so"
strip_runtime_files "${local_stage}/libdejavu_agent.so" "${local_stage}/libdejavu.so"
{
    cat "${HOOKX_LUA}"
    printf '\n'
    cat "${INIT_LUA}"
} > "${rendered_init}"

# Uploads precede the device lock, so their names must also be unique across hosts.
remote_suffix=$(python3 -c 'import secrets; print(secrets.randbits(128))')
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
"${adb_command[@]}" push "${local_stage}/libdejavu_agent.so" "${remote_tmp}" >/dev/null
"${adb_command[@]}" push "${local_stage}/libdejavu.so" "${remote_core_tmp}" >/dev/null
"${adb_command[@]}" push "${rendered_init}" "${remote_init_tmp}" >/dev/null

sed "s/__SUFFIX__/${remote_suffix}/g" > "${local_script}" <<'DEPLOY_SCRIPT'
set -e
module=/data/adb/modules/dejavu_zygisk
# Keep this inode stable: the descriptor holds the lock through EXIT rollback.
exec 9>>"${module}/.deploy.lock"
flock -n 9 9>&9 || { echo 'error: Agent deployment is busy; retry deployment' >&2; exit 1; }
suffix=__SUFFIX__
staging="${module}/.deploy.${suffix}"
backup="${module}/.deploy-backup.${suffix}"
# Never reuse a directory that may contain a retained recovery copy.
if [ -e "${staging}" ] || [ -L "${staging}" ] || \
    [ -e "${backup}" ] || [ -L "${backup}" ]; then
    echo "error: deployment staging or backup already exists for ${suffix}" >&2
    exit 1
fi
install_started=0
restore() {
    status=$?
    trap - EXIT
    set +e
    rollback_failed=0
    if [ "${status}" -ne 0 ] && [ "${install_started}" -eq 1 ]; then
        for target in \
            "${module}/lib/arm64-v8a/libdejavu.so" \
            "${module}/lib/arm64-v8a/libdejavu_agent.so" \
            "${module}/config/init.lua"; do
            saved="${backup}/$(basename "${target}")"
            if [ -f "${saved}" ]; then
                # Targets not yet replaced already reference the saved inode.
                if [ "${saved}" -ef "${target}" ]; then
                    continue
                fi
                if ! mv -f "${saved}" "${target}"; then
                    echo "error: rollback failed for ${target}; backup retained at ${saved}" >&2
                    rollback_failed=1
                fi
            elif ! rm -f "${target}"; then
                echo "error: rollback could not remove ${target}" >&2
                rollback_failed=1
            fi
        done
    fi
    if ! rm -rf "${staging}"; then
        echo "warning: staging cleanup failed: ${staging}" >&2
    fi
    if [ "${rollback_failed}" -eq 0 ]; then
        if ! rm -rf "${backup}"; then
            echo "warning: backup cleanup failed: ${backup}" >&2
        fi
    fi
    exit "${status}"
}
trap restore EXIT
mkdir -p "${staging}/lib/arm64-v8a" "${staging}/config"
cp "/data/local/tmp/libdejavu.so.${suffix}" "${staging}/lib/arm64-v8a/libdejavu.so"
cp "/data/local/tmp/libdejavu_agent.so.${suffix}" "${staging}/lib/arm64-v8a/libdejavu_agent.so"
cp "/data/local/tmp/dejavu-init.lua.${suffix}" "${staging}/config/init.lua"
chmod 0755 "${staging}/lib/arm64-v8a/libdejavu.so" "${staging}/lib/arm64-v8a/libdejavu_agent.so"
chmod 0644 "${staging}/config/init.lua"
chcon u:object_r:system_file:s0 "${staging}/lib/arm64-v8a/libdejavu.so" \
    "${staging}/lib/arm64-v8a/libdejavu_agent.so" "${staging}/config/init.lua"
mkdir -p "${backup}"
for target in \
    "${module}/lib/arm64-v8a/libdejavu.so" \
    "${module}/lib/arm64-v8a/libdejavu_agent.so" \
    "${module}/config/init.lua"; do
    if [ -f "${target}" ]; then
        # A hardlink preserves ownership, mode and SELinux context exactly.
        # Neither deployment nor rollback ever writes into a mapped inode.
        ln "${target}" "${backup}/$(basename "${target}")"
    fi
done
install_started=1
mv -f "${staging}/lib/arm64-v8a/libdejavu.so" "${module}/lib/arm64-v8a/libdejavu.so"
mv -f "${staging}/lib/arm64-v8a/libdejavu_agent.so" "${module}/lib/arm64-v8a/libdejavu_agent.so"
mv -f "${staging}/config/init.lua" "${module}/config/init.lua"
rm -f "/data/local/tmp/libdejavu.so.${suffix}"     "/data/local/tmp/libdejavu_agent.so.${suffix}"     "/data/local/tmp/dejavu-init.lua.${suffix}" "/data/local/tmp/dejavu-deploy.${suffix}.sh"
DEPLOY_SCRIPT
"${adb_command[@]}" push "${local_script}" "${deploy_script}" >/dev/null
"${adb_command[@]}" shell "su -c 'sh ${deploy_script}'"

if [[ "${NO_RESTART}" != "1" ]]; then
    "${adb_command[@]}" shell "am force-stop '${PROCESS}'; sleep 1; am start -W -n '${ACTIVITY}'"
fi
