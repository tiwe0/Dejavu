#!/usr/bin/env bash
set -euo pipefail
APP_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
JSON_JAR=${JSON_JAR:-"${APP_DIR}/../out/tooling/json-20250517.jar"}
TEST_DIR=$(mktemp -d "${TMPDIR:-/tmp}/dejavu-native-regression.XXXXXX")
trap 'rm -rf "${TEST_DIR}"' EXIT
printf 'native-host-only\n' > "${TEST_DIR}/.host-only"
mkdir "${TEST_DIR}/bin" "${TEST_DIR}/mapped-data"
for tool in chown chcon restorecon sha256sum getprop setprop base64 mv flock; do
    cp "${APP_DIR}/tests/native/fake-tool.py" "${TEST_DIR}/bin/${tool}"
    chmod 700 "${TEST_DIR}/bin/${tool}"
done
"${KOTLINC:-kotlinc}" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/NativePluginArtifact.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/NativePluginLibrary.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/NativePluginManifest.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/PluginPackage.kt" \
    "${APP_DIR}/tests/native/RootShell.kt" \
    "${APP_DIR}/tests/native/NativePluginLibraryTest.kt" \
    "${APP_DIR}/tests/importer/stubs/android/net/Uri.kt" \
    "${APP_DIR}/tests/importer/stubs/android/database/Cursor.kt" \
    "${APP_DIR}/tests/importer/stubs/android/content/Context.kt" \
    "${APP_DIR}/tests/importer/stubs/android/provider/DocumentsContract.kt" \
    "${APP_DIR}/tests/importer/MemoryDocumentProvider.kt" \
    -classpath "${JSON_JAR}" -include-runtime -d "${TEST_DIR}/native-tests.jar"
DEJAVU_NATIVE_SANDBOX="${TEST_DIR}" PATH="${TEST_DIR}/bin:${PATH}" \
    "${JAVA_HOME:+${JAVA_HOME}/bin/}java" -cp "${TEST_DIR}/native-tests.jar:${JSON_JAR}" \
    cafe.ivory.dejavu.data.NativePluginLibraryTestKt
