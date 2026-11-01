#!/usr/bin/env bash
# Automated on-device smoke test for the native/JIT/plugin lifecycle.
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
ADB=${ADB:-adb}
TARGET_PROCESS=${TARGET_PROCESS:-com.android.settings}
SERIAL=${ADB_SERIAL:-}
ADB_COMMAND=("${ADB}")
if [[ -n "${SERIAL}" ]]; then
    ADB_COMMAND+=(-s "${SERIAL}")
fi
FILES_DIR="/data/user_de/0/${TARGET_PROCESS}/files"
NATIVE_PLUGIN="${ROOT_DIR}/out/plugins/hello/libdejavu_hello.so"
HELLO_JIT="${ROOT_DIR}/plugins/hello/hello_jit.c"
HOOK_JIT="${ROOT_DIR}/plugins/hello/hello_hook_jit.c"
CALL_FILE="${ROOT_DIR}/out/tooling/plugin-device-call-atoi.c"
PUSH_CALL_FILE="/data/local/tmp/plugin-device-call-atoi.c"

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

cleanup() {
    "${ROOT_DIR}/dejavu" plugin -p "${TARGET_PROCESS}" allow --clear >/dev/null 2>&1 || true
    rm -f "${CALL_FILE}"
    "${ADB_COMMAND[@]}" shell "su -c 'rm -f \
        ${FILES_DIR}/libdejavu_hello.so ${FILES_DIR}/libdejavu_hello.so.SKILL.md \
        ${FILES_DIR}/hello_jit.c ${FILES_DIR}/hello_jit.c.SKILL.md \
        ${FILES_DIR}/hello_hook_jit.c ${FILES_DIR}/hello_hook_jit.c.SKILL.md \
        ${FILES_DIR}/plugin-device-call-atoi.c ${PUSH_CALL_FILE}'" >/dev/null 2>&1 || true
}
trap cleanup EXIT

cd "${ROOT_DIR}"
[[ -f "${NATIVE_PLUGIN}" ]] || fail "missing ${NATIVE_PLUGIN}; run plugins/hello/build.sh"
[[ -f "${HELLO_JIT}" ]] || fail "missing ${HELLO_JIT}"

echo "== native plugin load/call/unload"
"${ROOT_DIR}/dejavu" plugin -p "${TARGET_PROCESS}" install "${NATIVE_PLUGIN}" \
    --skill "${ROOT_DIR}/plugins/hello/SKILL.md" >/dev/null
"${ROOT_DIR}/dejavu" plugin -p "${TARGET_PROCESS}" call hello info | grep -q 'interfaces=mem,module,hook' \
    || fail "native hello info"
"${ROOT_DIR}/dejavu" plugin -p "${TARGET_PROCESS}" unload hello >/dev/null

echo "== JIT duplicate load/reload"
"${ROOT_DIR}/dejavu" plugin -p "${TARGET_PROCESS}" install "${HELLO_JIT}" >/dev/null
[[ "$("${ROOT_DIR}/dejavu" plugin -p "${TARGET_PROCESS}" call hellojit add --arg 1 --arg 2)" == "3" ]] \
    || fail "hellojit add"
"${ROOT_DIR}/dejavu" plugin -p "${TARGET_PROCESS}" jit "${FILES_DIR}/hello_jit.c" \
    | grep -q 'already loaded' || fail "duplicate JIT load was not rejected"
"${ROOT_DIR}/dejavu" plugin -p "${TARGET_PROCESS}" unload hellojit >/dev/null

echo "== hook index shift and prepare teardown"
"${ROOT_DIR}/dejavu" plugin -p "${TARGET_PROCESS}" install "${NATIVE_PLUGIN}" \
    --skill "${ROOT_DIR}/plugins/hello/SKILL.md" >/dev/null
"${ROOT_DIR}/dejavu" plugin -p "${TARGET_PROCESS}" install "${HOOK_JIT}" >/dev/null
[[ "$("${ROOT_DIR}/dejavu" plugin -p "${TARGET_PROCESS}" call hellohookjit install)" == "true" ]] \
    || fail "hook install"
"${ROOT_DIR}/dejavu" plugin -p "${TARGET_PROCESS}" unload hello >/dev/null
"${ROOT_DIR}/dejavu" plugin -p "${TARGET_PROCESS}" unload hellohookjit >/dev/null
"${ROOT_DIR}/dejavu" plugin -p "${TARGET_PROCESS}" allow --clear >/dev/null

cat > "${CALL_FILE}" <<'CALL_SOURCE'
void dejavu_native_call(void) {
    unsigned long size = 0;
    const char *value = dejavu_native_call_argument_string(0, &size);
    int (*target)(const char *) = (void *)(unsigned long long)DEJAVU_NATIVE_CALL_ADDRESS;
    dejavu_native_call_return((long long)target(value));
}
CALL_SOURCE
"${ADB_COMMAND[@]}" push "${CALL_FILE}" "${PUSH_CALL_FILE}" >/dev/null
"${ADB_COMMAND[@]}" shell "su -c 'cp ${PUSH_CALL_FILE} ${FILES_DIR}/plugin-device-call-atoi.c && chmod 644 ${FILES_DIR}/plugin-device-call-atoi.c'" >/dev/null
if [[ "$("${ROOT_DIR}/dejavu" native -p "${TARGET_PROCESS}" call-file libc.so atoi \
    "${FILES_DIR}/plugin-device-call-atoi.c" --arg s:42)" != "42" ]]; then
    fail "atoi hook was not removed"
fi

echo "PASS: plugin/JIT device lifecycle"
