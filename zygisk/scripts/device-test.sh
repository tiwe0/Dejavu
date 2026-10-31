#!/usr/bin/env bash
set -euo pipefail

# On-device acceptance test for the Dejavu Zygisk module.
#
# Requires: adb, a rooted device with a compatible Zygisk provider, the module
# installed and the target app installed. The script uses the module's file
# control channel (run/<process>.lua) and never reboots the device.
#
# Cases assert on the companion's logcat lines using a unique per-run token:
# control channel, writable generated data (SIGSEGV regression), an
# instance-method hook, a native function hook, oversized control responses and
# the documented header-include limitation. --with-hang-test additionally checks
# that a hung
# Lua eval no longer wedges the control channel forever.
#
# Usage:
#   TARGET_PROCESS=bin.mt.plus ./zygisk/scripts/device-test.sh
#   ADB_SERIAL=... TARGET_PROCESS=com.android.settings \
#       ./zygisk/scripts/device-test.sh --with-hang-test

ADB=${ADB:-adb}
ADB_SERIAL=${ADB_SERIAL:-}
MODULE_ID=${MODULE_ID:-dejavu_zygisk}
TARGET_PROCESS=${TARGET_PROCESS:-bin.mt.plus}
TARGET_ACTIVITY=${TARGET_ACTIVITY:-}
WITH_HANG_TEST=0
LOG_TIMEOUT=${LOG_TIMEOUT:-15}
AGENT_TIMEOUT=${AGENT_TIMEOUT:-25}

MODULE_DIR="/data/adb/modules/${MODULE_ID}"
RUN_DIR="${MODULE_DIR}/run"
RUN_TOKEN="devicetest$(date +%s)$$"
CASES_PASSED=0

usage() {
    sed -n '3,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

for argument in "$@"; do
    case "${argument}" in
        --with-hang-test) WITH_HANG_TEST=1 ;;
        -h|--help) usage; exit 0 ;;
        *)
            echo "error: unknown argument: ${argument}" >&2
            usage >&2
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

root_shell() {
    # Encode the command so quoting survives adb shell -> su -> /system/bin/sh.
    local command=$1
    local encoded
    encoded=$(printf '%s' "${command}" | base64 | tr -d '\n')
    adb_cmd shell "su -c 'echo ${encoded} | base64 -d | sh'"
}

root_shell_quiet() {
    root_shell "$1" >/dev/null 2>&1
}

logcat_dump() {
    adb_cmd logcat -d -b all 2>/dev/null || true
}

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

pass() {
    CASES_PASSED=$((CASES_PASSED + 1))
    echo "OK: $1"
}

target_pid() {
    adb_cmd shell "pidof ${TARGET_PROCESS}" 2>/dev/null | tr -d '\r' | head -n 1
}

launch_target() {
    adb_cmd shell "am force-stop ${TARGET_PROCESS}" >/dev/null 2>&1 || true
    # Wait for the previous process to disappear: its companion handler keeps
    # watching the run directory until then.
    local deadline=$((SECONDS + 10))
    while [[ -n "$(target_pid)" ]] && (( SECONDS < deadline )); do
        sleep 0.5
    done
    sleep 1
    if [[ -n "${TARGET_ACTIVITY}" ]]; then
        adb_cmd shell "am start -W -n ${TARGET_ACTIVITY}" >/dev/null 2>&1 ||
            return 1
    else
        adb_cmd shell "monkey -p ${TARGET_PROCESS} -c android.intent.category.LAUNCHER 1" \
            >/dev/null 2>&1 || return 1
    fi
}

wait_for_target() {
    local deadline=$((SECONDS + AGENT_TIMEOUT))
    while (( SECONDS < deadline )); do
        local pid
        pid=$(target_pid)
        if [[ -n "${pid}" ]] &&
            root_shell_quiet "test -f ${RUN_DIR}/${TARGET_PROCESS}.${pid}.rpc"; then
            sleep 1
            echo "  target pid=${pid}, agent endpoint ready"
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_for_log() {
    local pattern=$1
    local timeout=$2
    local deadline=$((SECONDS + timeout))
    while (( SECONDS < deadline )); do
        if logcat_dump | grep -Fq -- "${pattern}"; then
            return 0
        fi
        sleep 0.5
    done
    return 1
}

write_script() {
    # Pass the script through stdin/base64 so host-side temp paths never need
    # to be visible to adb (required when using the Windows adb binary). Write
    # atomically: the companion polls the run directory and must never observe
    # a truncated copy.
    local content=$1
    local payload
    payload=$(printf '%s\n' "${content}" | base64 | tr -d '\n')
    root_shell "printf '%s' '${payload}' | base64 -d > ${RUN_DIR}/.dejavu-device-test.tmp && chmod 600 ${RUN_DIR}/.dejavu-device-test.tmp && mv -f ${RUN_DIR}/.dejavu-device-test.tmp ${RUN_DIR}/${TARGET_PROCESS}.lua"
}

# Emit a unique token from Lua. Errors are reported by the companion with the
# Lua error message, which lets us assert on host-visible state without relying
# on the target process' own logcat output.
token_script() {
    local message=$1
    printf 'error("__TOKEN__ %s")' "${message}"
}

run_token_case() {
    local name=$1
    local message=$2
    local script
    script=$(token_script "${message}")
    script=${script//__TOKEN__/${RUN_TOKEN}}
    echo "== ${name}"
    write_script "${script}"
    if ! wait_for_log "${RUN_TOKEN} ${message}" "${LOG_TIMEOUT}"; then
        echo "--- recent Dejavu logcat ---" >&2
        logcat_dump | grep -iE 'Dejavu' | tail -n 20 >&2 || true
        fail "${name}: token was not observed"
    fi
    [[ -n "$(target_pid)" ]] || fail "${name}: target process died"
    pass "${name}"
}

cleanup() {
    root_shell_quiet "rm -f ${RUN_DIR}/${TARGET_PROCESS}.lua ${RUN_DIR}/.dejavu-device-test.tmp" || true
}
trap cleanup EXIT

echo "== preflight"
command -v "${ADB}" >/dev/null 2>&1 || fail "adb not found in PATH (set ADB=/path/to/adb)"
[[ "${TARGET_PROCESS}" =~ ^[A-Za-z0-9_.:-]+$ ]] ||
    fail "TARGET_PROCESS must match [A-Za-z0-9_.:-]+"
[[ "${MODULE_ID}" =~ ^[A-Za-z0-9_.-]+$ ]] ||
    fail "MODULE_ID must match [A-Za-z0-9_.-]+"
adb_cmd wait-for-device
device_state=$(adb_cmd get-state 2>/dev/null | tr -d '\r' || true)
[[ "${device_state}" == "device" ]] ||
    fail "no adb device (set ADB_SERIAL when multiple devices are attached)"
root_shell "id" | grep -q 'uid=0' ||
    fail "root is required (adb shell su -c id failed)"
root_shell_quiet "test -d ${MODULE_DIR}" ||
    fail "module ${MODULE_ID} is not installed at ${MODULE_DIR}"
root_shell_quiet "test -d ${RUN_DIR}" ||
    fail "module run directory is missing: ${RUN_DIR}"
pass "preflight"

echo "== configure target and restart ${TARGET_PROCESS}"
root_shell_quiet "rm -f ${RUN_DIR}/*.lua ${RUN_DIR}/${TARGET_PROCESS}.*.rpc" || true
root_shell "printf '%s\\n' ${TARGET_PROCESS} > ${MODULE_DIR}/config/targets.txt"
launch_target || fail "unable to launch ${TARGET_PROCESS} (set TARGET_ACTIVITY)"
wait_for_target || fail "agent for ${TARGET_PROCESS} did not become ready"
pass "target launch and agent attach"

run_token_case "control channel" "control-ok"

# Writable generated data: on the unfixed TinyCC this crashes the target
# process (SIGSEGV); with the runtime-safety patch the calls return 6 and 7.
echo "== writable generated data (H1 regression)"
write_script "local ok, fn = pcall(dejavu.compile, [[
int dejavu_global = 5;
int dejavu_entry(void) { dejavu_global += 1; return dejavu_global; }
]])
if not ok then error(\"${RUN_TOKEN} h1 compile-failed\") end
local ok1, first = pcall(fn)
local ok2, second = pcall(fn)
if not ok1 or not ok2 then error(\"${RUN_TOKEN} h1 call-failed\") end
if first ~= 6 or second ~= 7 then
    error(\"${RUN_TOKEN} h1 unexpected \" .. tostring(first) .. \"/\" .. tostring(second))
end
error(\"${RUN_TOKEN} h1 ok\")"
if ! wait_for_log "${RUN_TOKEN} h1 ok" "${LOG_TIMEOUT}"; then
    if [[ -z "$(target_pid)" ]]; then
        fail "writable generated data: target process crashed (H1 regression)"
    fi
    logcat_dump | grep -iE 'Dejavu' | tail -n 20 >&2 || true
    fail "writable generated data: expected success token not observed"
fi
pass "writable generated data (H1 regression)"

# Instance-method hook: install a hook on a boot-class instance method and
# make sure the target keeps running after interaction.
echo "== instance-method hook"
write_script "local source = [[
int before_hook(dejavu_hook_context *context) {
    (void)context;
    return DEJAVU_HOOK_OK;
}
]]
local ok, id = pcall(hook.install, \"java.lang.String\", \"length\", \"()I\", source)
if not ok then error(\"${RUN_TOKEN} instance failed\") end
error(\"${RUN_TOKEN} instance ok id=\" .. tostring(id))"
if ! wait_for_log "${RUN_TOKEN} instance ok" "${LOG_TIMEOUT}"; then
    logcat_dump | grep -iE 'Dejavu' | tail -n 20 >&2 || true
    fail "instance-method hook: install token not observed"
fi
adb_cmd shell "input swipe 540 1600 540 600 150" >/dev/null 2>&1 || true
sleep 1
[[ -n "$(target_pid)" ]] || fail "instance-method hook: target died after interaction"
pass "instance-method hook"

# Native function hook: hook a libc function used by the control channel
# itself, verify the replacement runs and calls the original, then unhook.
echo "== native function hook"
write_script "local source = [[
long dejavu_native_target(int fd, void *buffer, unsigned long count) {
    dejavu_native_hit();
    return ((long (*)(int, void *, unsigned long))dejavu_native_original())(fd, buffer, count);
}
]]
local ok, id = pcall(native.install, \"libc.so\", \"read\", source)
if not ok then error(\"${RUN_TOKEN} native-install-failed: \" .. tostring(id)) end
_G.DEVICE_TEST_NATIVE_ID = id
error(\"${RUN_TOKEN} native-installed\")"
if ! wait_for_log "${RUN_TOKEN} native-installed" "${LOG_TIMEOUT}"; then
    logcat_dump | grep -iE 'Dejavu' | tail -n 20 >&2 || true
    fail "native function hook: install token not observed"
fi
sleep 2
write_script "local id = _G.DEVICE_TEST_NATIVE_ID
if type(id) ~= \"number\" then error(\"${RUN_TOKEN} native-missing-id\") end
local hits = native.stats(id)
if type(hits) ~= \"number\" or hits <= 0 then
    error(\"${RUN_TOKEN} native-no-hits \" .. tostring(hits))
end
native.uninstall(id)
_G.DEVICE_TEST_NATIVE_ID = nil
error(\"${RUN_TOKEN} native-ok hits=\" .. tostring(hits))"
if ! wait_for_log "${RUN_TOKEN} native-ok" "${LOG_TIMEOUT}"; then
    logcat_dump | grep -iE 'Dejavu' | tail -n 20 >&2 || true
    fail "native function hook: hit/uninstall token not observed"
fi
[[ -n "$(target_pid)" ]] || fail "native function hook: target died"
pass "native function hook"

# Native hook error handling: a missing module/symbol must produce Lua errors.
echo "== native hook error handling"
write_script "local source = [[
int dejavu_native_target(void) { return 0; }
]]
local module_ok = pcall(native.install, 'libdejavu-not-loaded.so', 'foo', source)
local symbol_ok = pcall(native.install, 'libc.so', 'dejavu_missing_symbol_xyz', source)
if module_ok then error('${RUN_TOKEN} native-unexpected-module-success') end
if symbol_ok then error('${RUN_TOKEN} native-unexpected-symbol-success') end
error('${RUN_TOKEN} native-errors-ok')"
if ! wait_for_log "${RUN_TOKEN} native-errors-ok" "${LOG_TIMEOUT}"; then
    logcat_dump | grep -iE 'Dejavu' | tail -n 20 >&2 || true
    fail "native hook error handling: expected errors not observed"
fi
[[ -n "$(target_pid)" ]] || fail "native hook error handling: target died"
pass "native hook error handling"

# Repeated install/uninstall cycles exercise trampoline creation and cleanup.
echo "== native hook install/uninstall cycles"
write_script "local source = [[
long dejavu_native_target(int fd, void *buffer, unsigned long count) {
    dejavu_native_hit();
    return ((long (*)(int, void *, unsigned long))dejavu_native_original())(fd, buffer, count);
}
]]
for index = 1, 3 do
    local ok, value = pcall(native.install, 'libc.so', 'read', source)
    if not ok then error('${RUN_TOKEN} native-cycle-install ' .. index .. ': ' .. tostring(value)) end
    if native.uninstall(value) ~= true then
        error('${RUN_TOKEN} native-cycle-uninstall ' .. index)
    end
end
error('${RUN_TOKEN} native-cycles-ok')"
if ! wait_for_log "${RUN_TOKEN} native-cycles-ok" "${LOG_TIMEOUT}"; then
    logcat_dump | grep -iE 'Dejavu' | tail -n 25 >&2 || true
    fail "native hook install/uninstall cycles failed"
fi
[[ -n "$(target_pid)" ]] || fail "native hook cycles: target died"
pass "native hook install/uninstall cycles"

# Two simultaneous hooks plus native.clear().
echo "== native hook multiple + clear"
write_script "local read_source = [[
long dejavu_native_target(int fd, void *buffer, unsigned long count) {
    dejavu_native_hit();
    return ((long (*)(int, void *, unsigned long))dejavu_native_original())(fd, buffer, count);
}
]]
local write_source = [[
long dejavu_native_target(int fd, const void *buffer, unsigned long count) {
    dejavu_native_hit();
    return ((long (*)(int, const void *, unsigned long))dejavu_native_original())(fd, buffer, count);
}
]]
local read_ok = pcall(native.install, 'libc.so', 'read', read_source)
local write_ok = pcall(native.install, 'libc.so', 'write', write_source)
if not read_ok or not write_ok then error('${RUN_TOKEN} native-multi-install') end
local list = native.list()
if not string.find(list, 'libc.so#read', 1, true) or
    not string.find(list, 'libc.so#write', 1, true) then
    error('${RUN_TOKEN} native-multi-list')
end
local removed = native.clear()
if type(removed) ~= 'number' or removed < 2 then
    error('${RUN_TOKEN} native-multi-clear ' .. tostring(removed))
end
error('${RUN_TOKEN} native-multi-ok removed=' .. tostring(removed))"
if ! wait_for_log "${RUN_TOKEN} native-multi-ok" "${LOG_TIMEOUT}"; then
    logcat_dump | grep -iE 'Dejavu' | tail -n 25 >&2 || true
    fail "native hook multiple/clear failed"
fi
[[ -n "$(target_pid)" ]] || fail "native hook multiple: target died"
pass "native hook multiple + clear"

# Double uninstall must report an error instead of corrupting state.
echo "== native hook double uninstall"
write_script "local source = [[
long dejavu_native_target(int fd, void *buffer, unsigned long count) {
    dejavu_native_hit();
    return ((long (*)(int, void *, unsigned long))dejavu_native_original())(fd, buffer, count);
}
]]
local ok, value = pcall(native.install, 'libc.so', 'read', source)
if not ok then error('${RUN_TOKEN} native-double-install') end
if native.uninstall(value) ~= true then error('${RUN_TOKEN} native-double-first-uninstall') end
local second_ok = pcall(native.uninstall, value)
if second_ok then error('${RUN_TOKEN} native-double-unexpected-success') end
error('${RUN_TOKEN} native-double-ok')"
if ! wait_for_log "${RUN_TOKEN} native-double-ok" "${LOG_TIMEOUT}"; then
    logcat_dump | grep -iE 'Dejavu' | tail -n 20 >&2 || true
    fail "native hook double uninstall: expected error not observed"
fi
[[ -n "$(target_pid)" ]] || fail "native hook double uninstall: target died"
pass "native hook double uninstall"

# Oversized control response: must be reported as an error, not by dropping
# the connection; the following case proves the channel is still usable.
echo "== oversized control response (M1 regression)"
write_script 'return string.rep("A", 2 * 1024 * 1024)'
if ! wait_for_log 'control result exceeds the 1 MiB response limit' "${LOG_TIMEOUT}"; then
    fail "oversized control response: expected a clear companion error"
fi
pass "oversized control response"
run_token_case "control channel after oversized response" "oversized-recovered"

# Documented limitation: device hook sources cannot include headers.
echo "== device hook source include (documented limitation)"
write_script "local source = [[
#include \"dejavu_hook_utils.h\"
int before_hook(dejavu_hook_context *context) { (void)context; return DEJAVU_HOOK_OK; }
]]
local ok, id = pcall(hook.install, \"android.app.Activity\", \"onResume\", \"()V\", source)
if ok then error(\"${RUN_TOKEN} include unexpectedly-succeeded\") end
error(\"${RUN_TOKEN} include failed-as-expected: \" .. tostring(id))"
if ! wait_for_log "${RUN_TOKEN} include failed-as-expected" "${LOG_TIMEOUT}"; then
    logcat_dump | grep -iE 'Dejavu' | tail -n 20 >&2 || true
    fail "device hook source include: expected failure token not observed"
fi
if ! wait_for_log "include file 'dejavu_hook_utils.h' not found" "${LOG_TIMEOUT}"; then
    fail "device hook source include: expected documented compile error not observed"
fi
pass "device hook source include limitation"

if (( WITH_HANG_TEST )); then
    echo "== hanging eval + relay timeout (M2 regression, waits ~40s)"
    write_script 'while true do end'
    sleep 3
    write_script "$(token_script "${RUN_TOKEN} hang-recovered")"
    sleep 35
    if logcat_dump | grep -Fq -- "${RUN_TOKEN} hang-recovered"; then
        fail "hang test: follow-up script ran although the Agent should be blocked"
    fi
    launch_target || true
    wait_for_target || fail "hang test: agent did not recover after target restart"
    write_script "$(token_script "${RUN_TOKEN} hang-recovered")"
    if ! wait_for_log "${RUN_TOKEN} hang-recovered" "${LOG_TIMEOUT}"; then
        logcat_dump | grep -iE 'Dejavu' | tail -n 20 >&2 || true
        fail "hang test: control channel did not recover after target restart"
    fi
    pass "hang eval + relay timeout"
fi

echo
echo "PASS: device acceptance (${CASES_PASSED} cases) for ${TARGET_PROCESS} on ${MODULE_ID}"
