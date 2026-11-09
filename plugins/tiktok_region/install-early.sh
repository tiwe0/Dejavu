#!/usr/bin/env bash
# Deploy the tiktok-region hooks as an early autoload section of the DejaVu
# module config/init.lua (runs inside Application.attach, before TikTok's
# device_register / server.json fetch), and optionally install the Lua package
# into the DejaVu Console plugin library.
#
# Usage:
#   ./plugins/tiktok_region/install-early.sh              # install + restart
#   ./plugins/tiktok_region/install-early.sh --no-restart
#   ./plugins/tiktok_region/install-early.sh --restore    # undo init.lua change
#
# Environment: PROCESS (default com.zhiliaoapp.musically), ADB, ADB_SERIAL,
#              REMOTE_MODULE (default /data/adb/modules/dejavu_zygisk).
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
PACKAGE_DIR="${ROOT_DIR}/plugins/tiktok_region"
PROCESS=${PROCESS:-com.zhiliaoapp.musically}
ADB=${ADB:-adb}
ADB_SERIAL=${ADB_SERIAL:-}
REMOTE_MODULE=${REMOTE_MODULE:-/data/adb/modules/dejavu_zygisk}
MARKER="tiktok-region early autoload"
BACKUP_NAME="init.lua.bak-tiktok-region"

RESTORE=0
RESTART=1
DEPLOY_LIBRARY=1
for argument in "$@"; do
    case "${argument}" in
        --restore) RESTORE=1 ;;
        --no-restart) RESTART=0 ;;
        --no-library) DEPLOY_LIBRARY=0 ;;
        -h|--help)
            sed -n '2,17p' "${BASH_SOURCE[0]}"
            exit 0
            ;;
        *)
            echo "unknown option: ${argument}" >&2
            exit 2
            ;;
    esac
done

adb_cmd() {
    if [[ -n "${ADB_SERIAL}" ]]; then
        "${ADB}" -s "${ADB_SERIAL}" "$@"
    else
        "${ADB}" "$@"
    fi
}

root_sh() {
    adb_cmd shell "su -c \"$1\""
}

restart_target() {
    adb_cmd shell am force-stop "${PROCESS}"
    adb_cmd shell monkey -p "${PROCESS}" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1 || true
}

WORK_DIR=$(mktemp -d)
trap 'rm -rf "${WORK_DIR}"' EXIT

if (( RESTORE )); then
    if ! adb_cmd shell "su -c \"test -f ${REMOTE_MODULE}/config/${BACKUP_NAME}\""; then
        echo "no backup found: ${REMOTE_MODULE}/config/${BACKUP_NAME}" >&2
        exit 1
    fi
    root_sh "cp -f ${REMOTE_MODULE}/config/${BACKUP_NAME} ${REMOTE_MODULE}/config/init.lua && chmod 644 ${REMOTE_MODULE}/config/init.lua && chcon u:object_r:system_file:s0 ${REMOTE_MODULE}/config/init.lua"
    echo "restored ${REMOTE_MODULE}/config/init.lua from ${BACKUP_NAME}"
    if (( RESTART )); then
        restart_target
        echo "restarted ${PROCESS}"
    fi
    exit 0
fi

# 1. Keep the pristine init.lua as the backup/base, even when re-running.
if adb_cmd shell "su -c \"test -f ${REMOTE_MODULE}/config/${BACKUP_NAME}\""; then
    adb_cmd shell "su -c \"cat ${REMOTE_MODULE}/config/${BACKUP_NAME}\"" > "${WORK_DIR}/base.lua"
else
    adb_cmd shell "su -c \"cat ${REMOTE_MODULE}/config/init.lua\"" > "${WORK_DIR}/base.lua"
    adb_cmd push "${WORK_DIR}/base.lua" /data/local/tmp/init.lua.tiktok-region.base >/dev/null
    root_sh "cp -f /data/local/tmp/init.lua.tiktok-region.base ${REMOTE_MODULE}/config/${BACKUP_NAME} && chmod 644 ${REMOTE_MODULE}/config/${BACKUP_NAME} && chcon u:object_r:system_file:s0 ${REMOTE_MODULE}/config/${BACKUP_NAME}"
fi
if grep -q "${MARKER}" "${WORK_DIR}/base.lua"; then
    echo "error: base init.lua already contains the generated section; rerun after --restore" >&2
    exit 1
fi

# 2. Compose init.lua = pristine base + generated loader that embeds entry.lua.
python3 - "${PACKAGE_DIR}/entry.lua" "${WORK_DIR}/base.lua" "${WORK_DIR}/init.lua" "${MARKER}" <<'PY'
import pathlib
import sys

entry_path, base_path, out_path, marker = sys.argv[1:5]
entry = pathlib.Path(entry_path).read_text(encoding="utf-8")
base = pathlib.Path(base_path).read_text(encoding="utf-8")
section = """
-- ===== {marker} (generated; remove with install-early.sh --restore) =====
pcall(function()
    local definition = [===[
{entry}
]===]
    local chunk = assert(load(definition, "@tiktok-region/entry.lua", "t", _G))
    local api = chunk()
    if type(api) == "table" and type(api.start) == "function" then
        local ok, result = pcall(api.start, {{ session = "early-autoload" }})
        if not ok then
            pcall(hook.log, "tiktok-region: early start failed: " .. tostring(result))
        end
    else
        pcall(hook.log, "tiktok-region: early entry has no start()")
    end
end)
-- ===== end {marker} =====
""".format(marker=marker, entry=entry.rstrip("\n"))
pathlib.Path(out_path).write_text(base.rstrip("\n") + "\n" + section, encoding="utf-8")
PY

adb_cmd push "${WORK_DIR}/init.lua" /data/local/tmp/init.lua.tiktok-region >/dev/null
root_sh "cp -f /data/local/tmp/init.lua.tiktok-region ${REMOTE_MODULE}/config/init.lua && chmod 644 ${REMOTE_MODULE}/config/init.lua && chcon u:object_r:system_file:s0 ${REMOTE_MODULE}/config/init.lua"
echo "installed early autoload section into ${REMOTE_MODULE}/config/init.lua"

# 3. Install the Lua package into the Console directory-plugin library.
if (( DEPLOY_LIBRARY )); then
    LIBRARY=/data/adb/dejavu/plugins/tiktok-region
    adb_cmd shell "su -c \"mkdir -p ${LIBRARY}\""
    adb_cmd push "${PACKAGE_DIR}/manifest.json" /data/local/tmp/tiktok-region.manifest.json >/dev/null
    adb_cmd push "${PACKAGE_DIR}/entry.lua" /data/local/tmp/tiktok-region.entry.lua >/dev/null
    adb_cmd push "${PACKAGE_DIR}/SKILL.md" /data/local/tmp/tiktok-region.SKILL.md >/dev/null
    root_sh "cp -f /data/local/tmp/tiktok-region.manifest.json ${LIBRARY}/manifest.json && cp -f /data/local/tmp/tiktok-region.entry.lua ${LIBRARY}/entry.lua && cp -f /data/local/tmp/tiktok-region.SKILL.md ${LIBRARY}/SKILL.md && chown -R 0:0 ${LIBRARY} && chmod 700 ${LIBRARY} && chmod 600 ${LIBRARY}/manifest.json ${LIBRARY}/entry.lua ${LIBRARY}/SKILL.md"
    echo "installed Lua package into ${LIBRARY}"
fi

# 4. Restart the target so the early hook runs before device_register.
if (( RESTART )); then
    restart_target
    echo "restarted ${PROCESS}"
    echo "check: adb logcat -d | grep -E 'tiktok-region|DejavuHook'"
fi
