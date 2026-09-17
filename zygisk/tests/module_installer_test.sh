#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
INSTALLER="${ROOT_DIR}/module/customize.sh"

run_installer() {
    local arch=$1
    local api=$2
    local expected_status=$3
    local expected_message=${4:-}
    local module_dir
    local output
    local status

    module_dir=$(mktemp -d)
    set +e
    output=$(
        ARCH="${arch}" \
        API="${api}" \
        MODPATH="${module_dir}" \
        sh -c '
            ui_print() { :; }
            abort() { printf "%s\n" "$*" >&2; exit 64; }
            set_perm_recursive() { :; }
            set_perm() { :; }
            chcon() { :; }
            . "$1"
        ' sh "${INSTALLER}" 2>&1
    )
    status=$?
    set -e
    rm -rf "${module_dir}"

    if [[ "${status}" -ne "${expected_status}" ]]; then
        printf 'installer status mismatch: arch=%s api=%s expected=%s actual=%s\n' \
            "${arch}" "${api}" "${expected_status}" "${status}" >&2
        printf '%s\n' "${output}" >&2
        exit 1
    fi
    if [[ -n "${expected_message}" && "${output}" != *"${expected_message}"* ]]; then
        printf 'installer message mismatch: expected %q in %q\n' \
            "${expected_message}" "${output}" >&2
        exit 1
    fi
}

run_installer x86_64 26 64 "currently supports arm64 only"
run_installer arm64 25 64 "supports Android 8 through 15"
run_installer arm64 26 0
run_installer arm64 35 0
run_installer arm64 36 64 "supports Android 8 through 15"

echo "OK: module installer Android version boundaries"
