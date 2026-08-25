#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
ADB_SERIAL=${ADB_SERIAL:-}
PROCESS=${PROCESS:-bin.mt.plus}
SCRIPT=${1:?usage: push-control.sh SCRIPT [PROCESS]}
if [[ $# -ge 2 ]]; then
    PROCESS=$2
fi

if [[ ! -f "${SCRIPT}" ]]; then
    echo "error: control script not found: ${SCRIPT}" >&2
    exit 1
fi
if [[ ! "${PROCESS}" =~ ^[A-Za-z0-9._:-]+$ ]]; then
    echo "error: invalid process name: ${PROCESS}" >&2
    exit 1
fi

adb_command=(adb)
if [[ -n "${ADB_SERIAL}" ]]; then
    adb_command+=(-s "${ADB_SERIAL}")
fi

remote_tmp="/data/local/tmp/dejavu-control.$$"
remote_run="/data/adb/modules/dejavu_zygisk/run"
remote_target="${remote_run}/${PROCESS}.lua"
"${adb_command[@]}" push "${SCRIPT}" "${remote_tmp}"
"${adb_command[@]}" shell "su -c 'mkdir -p ${remote_run} && chmod 0755 ${remote_run} && cp ${remote_tmp} ${remote_target}.new && chmod 0644 ${remote_target}.new && mv -f ${remote_target}.new ${remote_target} && rm -f ${remote_tmp}'"
echo "OK: pushed live control to ${PROCESS}"
