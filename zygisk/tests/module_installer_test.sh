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
    local fixture=${5:-complete}
    local status

    module_dir=$(mktemp -d)
    mkdir -p "$module_dir/bin"
    cp "$ROOT_DIR/module/router-install.sh" "$module_dir/router-install.sh"
    if [[ "$fixture" == complete ]]; then
        printf '#!/bin/sh\nexit 0\n' > "$module_dir/bin/dejavuerouter"
        chmod 0755 "$module_dir/bin/dejavuerouter"
    fi
    set +e
    output=$(
        ARCH="${arch}" \
        API="${api}" \
        MODPATH="${module_dir}" \
        DEJAVU_DIR="${module_dir}/runtime" \
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
    if [[ "$expected_status" -eq 0 && "$status" -eq 0 ]]; then
        cmp "$module_dir/bin/dejavuerouter" "$module_dir/runtime/bin/dejavuerouter"
        test -x "$module_dir/runtime/bin/dejavuerouter"
    fi
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

run_installer arm64 26 64 "Unable to deploy internal Dejavu router" missing

echo "OK: module installer Android boundaries and internal router deployment"

# A framework update must retain the migrated user's routes/scopes/trust setup.
fixture=$(mktemp -d)
trap 'rm -rf "$fixture"' EXIT
previous="$fixture/previous"
next="$fixture/next"
mkdir -p "$previous/config/early-hooks" "$next/bin" "$next/config"
cp "$ROOT_DIR/module/router-install.sh" "$next/router-install.sh"
printf '#!/bin/sh\nexit 0\n' > "$next/bin/dejavuerouter"
chmod 0755 "$next/bin/dejavuerouter"
printf 'com.example.target\n' > "$previous/config/targets.txt"
printf '/data/user/0/com.example.target/files/plugin.so\n' > "$previous/config/plugins.txt"
printf 'return 42\n' > "$previous/config/early-hooks/com.example.target.lua"
printf 'debug.dejavu.plugins.allowed.0=/data/user/0/com.example.target/files/plugin.so\n' > "$previous/system.prop"
printf 'old bootstrap\n' > "$previous/config/init.lua"
printf 'new bootstrap\n' > "$next/config/init.lua"
ARCH=arm64 API=26 MODPATH="$next" DEJAVU_DIR="$fixture/runtime" \
DEJAVU_PREVIOUS_MODULE_DIR="$previous" sh -c '
    ui_print() { :; }
    abort() { printf "%s\n" "$*" >&2; exit 64; }
    set_perm_recursive() { :; }
    set_perm() { :; }
    chcon() { :; }
    . "$1"
' sh "$INSTALLER"
for relative in config/targets.txt config/plugins.txt config/early-hooks/com.example.target.lua system.prop; do
    cmp "$previous/$relative" "$next/$relative"
done
test "$(cat "$next/config/init.lua")" = 'new bootstrap'
echo 'OK: module upgrades preserve device configuration and refresh the framework bootstrap'
