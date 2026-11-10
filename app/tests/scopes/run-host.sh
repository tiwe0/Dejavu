#!/usr/bin/env bash
set -euo pipefail
APP_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
JSON_JAR=${JSON_JAR:-"${APP_DIR}/../out/tooling/json-20250517.jar"}
TEST_DIR=$(mktemp -d "${TMPDIR:-/tmp}/dejavu-scopes.XXXXXX")
trap 'rm -rf "${TEST_DIR}"' EXIT
printf 'scopes-host-only\n' > "${TEST_DIR}/.host-only"
mkdir "${TEST_DIR}/bin" "${TEST_DIR}/data"
for tool in chown sha256sum mv flock; do
    cp "${APP_DIR}/tests/scopes/fake-tool.py" "${TEST_DIR}/bin/${tool}"
    chmod 700 "${TEST_DIR}/bin/${tool}"
done
"${KOTLINC:-kotlinc}" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/PluginScopeStore.kt" \
    "${APP_DIR}/tests/scopes/RootShell.kt" \
    "${APP_DIR}/tests/scopes/PluginScopeStoreTest.kt" \
    -classpath "${JSON_JAR}" -include-runtime -d "${TEST_DIR}/tests.jar"
DEJAVU_SCOPE_SANDBOX="${TEST_DIR}" PATH="${TEST_DIR}/bin:${PATH}" \
    "${JAVA_HOME:+${JAVA_HOME}/bin/}java" -cp "${TEST_DIR}/tests.jar:${JSON_JAR}" \
    cafe.ivory.dejavu.data.PluginScopeStoreTestKt
