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
# instance-method hook, native function hooks and active calls (including typed
# arguments), Java active invocation (static/constructor/instance), chained
# results, filesystem and module/symbol helpers, oversized control responses and
# the documented header-include limitation.
# --with-hang-test additionally checks that a hung
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
            >/dev/null 2>&1 || true
        # `monkey` occasionally injects the launch event without starting the
        # process; fall back to the resolved launcher activity.
        sleep 2
        if [[ -z "$(target_pid)" ]]; then
            local resolved
            resolved=$(adb_cmd shell \
                "cmd package resolve-activity --brief -c android.intent.category.LAUNCHER ${TARGET_PROCESS}" \
                2>/dev/null | tr -d '\r' | tail -n 1)
            if [[ -n "${resolved}" ]]; then
                adb_cmd shell "am start -W -n ${resolved}" >/dev/null 2>&1 || true
            fi
        fi
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

# Active native calls: invoke exported functions through a TinyCC wrapper and
# verify arguments/results, then use them to trigger a native hook reliably.
echo "== native call (getpid)"
write_script "local source = [[
void dejavu_native_call(void) {
    int (*target)(void) = (void *)(unsigned long long)DEJAVU_NATIVE_CALL_ADDRESS;
    dejavu_native_call_return((long long)target());
}
]]
local pid = native.call('libc.so', 'getpid', source)
if type(pid) ~= 'number' or pid <= 0 then
    error('${RUN_TOKEN} native-call-getpid-failed ' .. tostring(pid))
end
error('${RUN_TOKEN} native-call-getpid-ok pid=' .. tostring(pid))"
if ! wait_for_log "${RUN_TOKEN} native-call-getpid-ok" "${LOG_TIMEOUT}"; then
    logcat_dump | grep -iE 'Dejavu' | tail -n 20 >&2 || true
    fail "native call getpid: expected result not observed"
fi
[[ -n "$(target_pid)" ]] || fail "native call getpid: target died"
pass "native call (getpid)"

echo "== native call (arguments)"
write_script "local source = [[
void dejavu_native_call(void) {
    int (*target)(int) = (void *)(unsigned long long)DEJAVU_NATIVE_CALL_ADDRESS;
    int value = (int)dejavu_native_call_argument(0);
    dejavu_native_call_return((long long)target(value));
}
]]
local result = native.call('libc.so', 'abs', source, -42)
if result ~= 42 then error('${RUN_TOKEN} native-call-abs-failed ' .. tostring(result)) end
error('${RUN_TOKEN} native-call-abs-ok')"
if ! wait_for_log "${RUN_TOKEN} native-call-abs-ok" "${LOG_TIMEOUT}"; then
    logcat_dump | grep -iE 'Dejavu' | tail -n 20 >&2 || true
    fail "native call arguments: expected abs(-42)=42"
fi
pass "native call (arguments)"

echo "== native call triggers hook"
write_script "local hook_source = [[
int dejavu_native_target(void) {
    dejavu_native_hit();
    return ((int (*)(void))dejavu_native_original())();
}
]]
local hook_ok, value = pcall(native.install, 'libc.so', 'getpid', hook_source)
if not hook_ok then error('${RUN_TOKEN} native-call-trigger-install') end
local call_source = [[
void dejavu_native_call(void) {
    int (*target)(void) = (void *)(unsigned long long)DEJAVU_NATIVE_CALL_ADDRESS;
    dejavu_native_call_return((long long)target());
}
]]
local pid = native.call('libc.so', 'getpid', call_source)
local hits = native.stats(value)
native.uninstall(value)
if type(hits) ~= 'number' or hits <= 0 then
    error('${RUN_TOKEN} native-call-trigger-no-hits ' .. tostring(hits))
end
error('${RUN_TOKEN} native-call-trigger-ok hits=' .. tostring(hits))"
if ! wait_for_log "${RUN_TOKEN} native-call-trigger-ok" "${LOG_TIMEOUT}"; then
    logcat_dump | grep -iE 'Dejavu' | tail -n 20 >&2 || true
    fail "native call trigger: hook was not hit"
fi
[[ -n "$(target_pid)" ]] || fail "native call trigger: target died"
pass "native call triggers hook"

echo "== native call error handling"
write_script "local source = [[
void dejavu_native_call(void) {
    dejavu_native_call_return(0);
}
]]
local module_ok = pcall(native.call, 'libdejavu-not-loaded.so', 'foo', source)
local symbol_ok = pcall(native.call, 'libc.so', 'dejavu_missing_symbol_xyz', source)
if module_ok or symbol_ok then
    error('${RUN_TOKEN} native-call-errors-unexpected-success')
end
error('${RUN_TOKEN} native-call-errors-ok')"
if ! wait_for_log "${RUN_TOKEN} native-call-errors-ok" "${LOG_TIMEOUT}"; then
    logcat_dump | grep -iE 'Dejavu' | tail -n 20 >&2 || true
    fail "native call error handling: expected errors not observed"
fi
pass "native call error handling"

# Typed argument protocol: strings and doubles go through the same Lua -> C
# conversion rules as the rest of the invoke APIs.
echo "== native call typed arguments"
write_script "local strlen_source = [[
void dejavu_native_call(void) {
    unsigned long size = 0;
    const char *text = dejavu_native_call_argument_string(0, &size);
    if (text == 0 || size != 6) { dejavu_native_call_return(-1); return; }
    int (*target)(const char *) = (void *)(unsigned long long)DEJAVU_NATIVE_CALL_ADDRESS;
    dejavu_native_call_return((long long)target(text));
}
]]
local length = native.call('libc.so', 'strlen', strlen_source, 'dejavu')
if length ~= 6 then error('${RUN_TOKEN} native-call-string-failed ' .. tostring(length)) end
local sqrt_source = [[
void dejavu_native_call(void) {
    double value = dejavu_native_call_argument_double(0);
    double (*target)(double) = (void *)(unsigned long long)DEJAVU_NATIVE_CALL_ADDRESS;
    dejavu_native_call_return((long long)(target(value) * 1000.0));
}
]]
local scaled = native.call('libm.so', 'sqrt', sqrt_source, 81.0)
if scaled ~= 9000 then error('${RUN_TOKEN} native-call-double-failed ' .. tostring(scaled)) end
error('${RUN_TOKEN} native-call-typed-ok')"
if ! wait_for_log "${RUN_TOKEN} native-call-typed-ok" "${LOG_TIMEOUT}"; then
    logcat_dump | grep -iE 'Dejavu' | tail -n 20 >&2 || true
    fail "native call typed arguments failed"
fi
[[ -n "$(target_pid)" ]] || fail "native call typed arguments: target died"
pass "native call typed arguments"

echo "== java invoke static conversions"
write_script "local text = java.invoke('java.lang.String', 'valueOf', '(I)Ljava/lang/String;', 42)
local number = java.invoke('java.lang.Integer', 'parseInt', '(Ljava/lang/String;)I', '123')
local big = java.invoke('java.lang.Long', 'parseLong', '(Ljava/lang/String;)J', '9007199254740993')
local decimal = java.invoke('java.lang.Double', 'parseDouble', '(Ljava/lang/String;)D', '2.5')
local flag = java.invoke('java.lang.Boolean', 'parseBoolean', '(Ljava/lang/String;)Z', 'true')
if text ~= '42' or number ~= 123 or big ~= 9007199254740993 or
    decimal ~= 2.5 or flag ~= true then
    error('${RUN_TOKEN} java-invoke-static-failed')
end
error('${RUN_TOKEN} java-invoke-static-ok')"
if ! wait_for_log "${RUN_TOKEN} java-invoke-static-ok" "${LOG_TIMEOUT}"; then
    logcat_dump | grep -iE 'Dejavu' | tail -n 20 >&2 || true
    fail "java invoke static conversions failed"
fi
pass "java invoke static conversions"

echo "== java constructor/instance/release"
write_script "local handle = java.new('java.lang.StringBuilder', '(Ljava/lang/String;)V', 'abc')
local text = java.invoke(handle, 'toString', '()Ljava/lang/String;')
if text ~= 'abc' then error('${RUN_TOKEN} java-instance-failed ' .. tostring(text)) end
if java.release(handle) ~= true then error('${RUN_TOKEN} java-release-failed') end
local after_release = pcall(java.invoke, handle, 'toString', '()Ljava/lang/String;')
if after_release then error('${RUN_TOKEN} java-release-use-after-free') end
error('${RUN_TOKEN} java-instance-ok')"
if ! wait_for_log "${RUN_TOKEN} java-instance-ok" "${LOG_TIMEOUT}"; then
    logcat_dump | grep -iE 'Dejavu' | tail -n 20 >&2 || true
    fail "java constructor/instance/release failed"
fi
pass "java constructor/instance/release"

echo "== java invoke error handling"
write_script "local missing = pcall(java.invoke, 'java.lang.String', 'noSuchMethod', '()V')
local wrong_type = pcall(java.invoke, 'java.lang.Integer', 'parseInt', '(Ljava/lang/String;)I', 123)
local unknown_class = pcall(java.invoke, 'com.example.NoSuchClass', 'foo', '()V')
if missing or wrong_type or unknown_class then
    error('${RUN_TOKEN} java-errors-unexpected-success')
end
error('${RUN_TOKEN} java-errors-ok')"
if ! wait_for_log "${RUN_TOKEN} java-errors-ok" "${LOG_TIMEOUT}"; then
    logcat_dump | grep -iE 'Dejavu' | tail -n 20 >&2 || true
    fail "java invoke error handling failed"
fi
pass "java invoke error handling"

# Invocation results must be reusable: nested calls, returned strings and
# object/array handles feed the next invocation.
echo "== java chained invocation results"
write_script "local nested = java.invoke(
    java.new('java.lang.StringBuilder', '(Ljava/lang/String;)V', 'abcd'),
    'length', '()I')
local builder = java.new('java.lang.StringBuilder', '(Ljava/lang/String;)V', '123')
local text = java.invoke(builder, 'toString', '()Ljava/lang/String;')
java.release(builder)
local parsed = java.invoke('java.lang.Integer', 'parseInt', '(Ljava/lang/String;)I', text)
local string_handle = java.new('java.lang.String', '(Ljava/lang/String;)V', 'a,b,c')
local parts = java.invoke(string_handle, 'split', '(Ljava/lang/String;)[Ljava/lang/String;', ',')
local count = java.invoke('java.lang.reflect.Array', 'getLength', '(Ljava/lang/Object;)I', parts)
java.release(parts)
java.release(string_handle)
if nested ~= 4 or parsed ~= 123 or count ~= 3 then
    error('${RUN_TOKEN} java-chain-failed ' .. tostring(nested) .. '/' ..
          tostring(parsed) .. '/' .. tostring(count))
end
error('${RUN_TOKEN} java-chain-ok')"
if ! wait_for_log "${RUN_TOKEN} java-chain-ok" "${LOG_TIMEOUT}"; then
    logcat_dump | grep -iE 'Dejavu' | tail -n 20 >&2 || true
    fail "java chained invocation results failed"
fi
pass "java chained invocation results"

echo "== cross-API chained invocation results"
write_script "local getpid_source = [[
void dejavu_native_call(void) {
    int (*target)(void) = (void *)(unsigned long long)DEJAVU_NATIVE_CALL_ADDRESS;
    dejavu_native_call_return((long long)target());
}
]]
local pid = native.call('libc.so', 'getpid', getpid_source)
local pid_text = java.invoke('java.lang.Long', 'toString', '(J)Ljava/lang/String;', pid)
if pid_text ~= tostring(pid) then error('${RUN_TOKEN} native-java-chain-failed') end
local abs_source = [[
void dejavu_native_call(void) {
    int (*target)(int) = (void *)(unsigned long long)DEJAVU_NATIVE_CALL_ADDRESS;
    dejavu_native_call_return((long long)target((int)dejavu_native_call_argument_int(0)));
}
]]
local value = java.invoke('java.lang.Math', 'abs', '(I)I', -7)
local result = native.call('libc.so', 'abs', abs_source, value)
if result ~= 7 then error('${RUN_TOKEN} java-native-chain-failed ' .. tostring(result)) end
error('${RUN_TOKEN} cross-api-chain-ok')"
if ! wait_for_log "${RUN_TOKEN} cross-api-chain-ok" "${LOG_TIMEOUT}"; then
    logcat_dump | grep -iE 'Dejavu' | tail -n 20 >&2 || true
    fail "cross-API chained invocation results failed"
fi
pass "cross-API chained invocation results"

# Agent helpers: file IO in the target app sandbox and library/symbol tools.
echo "== fs helpers"
write_script "local app = java.invoke('android.app.ActivityThread', 'currentApplication',
    '()Landroid/app/Application;')
local files = java.invoke(app, 'getFilesDir', '()Ljava/io/File;')
local dir = java.invoke(files, 'getAbsolutePath', '()Ljava/lang/String;')
java.release(files)
java.release(app)
local path = dir .. '/dejavu-helper-test.txt'
local written = fs.write(path, 'hello')
local content = fs.read(path)
local exists = fs.exists(path)
local stat = fs.stat(path)
local listing = fs.list(dir)
local removed = fs.remove(path)
local gone = fs.exists(path)
if written ~= 5 or content ~= 'hello' or exists ~= true or
    removed ~= true or gone ~= false or
    stat:find('size=5', 1, true) == nil or
    listing:find('dejavu-helper-test.txt', 1, true) == nil then
    error('${RUN_TOKEN} fs-helpers-failed')
end
error('${RUN_TOKEN} fs-helpers-ok')"
if ! wait_for_log "${RUN_TOKEN} fs-helpers-ok" "${LOG_TIMEOUT}"; then
    logcat_dump | grep -iE 'Dejavu' | tail -n 20 >&2 || true
    fail "fs helpers failed"
fi
pass "fs helpers"

echo "== module helpers"
write_script "local id = module.load('libz.so')
if type(id) ~= 'number' or id <= 0 then error('${RUN_TOKEN} module-load-failed') end
local symbols = module.symbols(id, 'inflate')
local version = module.resolve(id, 'zlibVersion')
local loaded = module.loaded()
local unloaded = module.unload(id)
local loaded_after = module.loaded()
if symbols:find('inflate', 1, true) == nil or
    type(version) ~= 'number' or version <= 0 or
    loaded:find('libz.so', 1, true) == nil or
    unloaded ~= true or
    loaded_after:find(tostring(id) .. '\\t', 1, true) ~= nil then
    error('${RUN_TOKEN} module-helpers-failed')
end
error('${RUN_TOKEN} module-helpers-ok')"
if ! wait_for_log "${RUN_TOKEN} module-helpers-ok" "${LOG_TIMEOUT}"; then
    logcat_dump | grep -iE 'Dejavu' | tail -n 20 >&2 || true
    fail "module helpers failed"
fi
pass "module helpers"

echo "== hidden symbol resolve/call"
write_script "local address = module.resolve('libc.so', '__getpid')
if type(address) ~= 'number' or address <= 0 then
    error('${RUN_TOKEN} hidden-resolve-failed')
end
local source = [[
void dejavu_native_call(void) {
    int (*target)(void) = (void *)(unsigned long long)DEJAVU_NATIVE_CALL_ADDRESS;
    dejavu_native_call_return((long long)target());
}
]]
local pid = native.call('libc.so', '__getpid', source)
if type(pid) ~= 'number' or pid <= 0 then error('${RUN_TOKEN} hidden-call-failed') end
local load_error = pcall(module.load, 'libdejavu-not-exist.so')
local symbols_error = pcall(module.symbols, 'libdejavu-not-loaded.so')
local id_error = pcall(module.resolve, 9999, 'x')
if load_error or symbols_error or id_error then
    error('${RUN_TOKEN} helper-errors-unexpected-success')
end
error('${RUN_TOKEN} hidden-symbol-ok')"
if ! wait_for_log "${RUN_TOKEN} hidden-symbol-ok" "${LOG_TIMEOUT}"; then
    logcat_dump | grep -iE 'Dejavu' | tail -n 20 >&2 || true
    fail "hidden symbol resolve/call failed"
fi
[[ -n "$(target_pid)" ]] || fail "hidden symbol resolve/call: target died"
pass "hidden symbol resolve/call"

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
