#!/usr/bin/env bash
set -euo pipefail

APP_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
JSON_JAR=${JSON_JAR:-"${APP_DIR}/../out/tooling/json-20250517.jar"}
if [[ ! -f "${JSON_JAR}" ]]; then
    echo "Missing host-test org.json jar; set JSON_JAR (see app/README.md)." >&2
    exit 1
fi
# Production RootShell selects these absolute paths before PATH. Refuse to run
# if it could bypass our fake su; this test must never invoke real elevation.
for candidate in /system/bin/su /debug_ramdisk/su /data/adb/ksu/bin/su /sbin/su; do
    if [[ -x "${candidate}" ]]; then
        echo "Cannot safely isolate importer test: executable ${candidate} exists." >&2
        exit 1
    fi
done

TEST_DIR=$(mktemp -d "${TMPDIR:-/tmp}/dejavu-importer-regression.XXXXXX")
trap 'rm -rf "${TEST_DIR}"' EXIT
printf 'dejavu-importer-host-only\n' > "${TEST_DIR}/.host-only"
mkdir "${TEST_DIR}/bin" "${TEST_DIR}/mapped-data"
for tool in su chown base64 mv; do
    cp "${APP_DIR}/tests/importer/fake-tool.py" "${TEST_DIR}/bin/${tool}"
    chmod 700 "${TEST_DIR}/bin/${tool}"
done

"${KOTLINC:-kotlinc}" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/PluginPackage.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/PluginPage.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/PluginZipArchive.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/LuaPackageScripts.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/PluginPackageImporter.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/RootShell.kt" \
    "${APP_DIR}/tests/importer/stubs/android/net/Uri.kt" \
    "${APP_DIR}/tests/importer/stubs/android/database/Cursor.kt" \
    "${APP_DIR}/tests/importer/stubs/android/content/Context.kt" \
    "${APP_DIR}/tests/importer/stubs/android/provider/DocumentsContract.kt" \
    "${APP_DIR}/tests/importer/MemoryDocumentProvider.kt" \
    "${APP_DIR}/tests/importer/PluginPackageImporterTest.kt" \
    -classpath "${JSON_JAR}" -include-runtime -d "${TEST_DIR}/importer-tests.jar"

DEJAVU_REPO_ROOT="${APP_DIR}/.." \
DEJAVU_IMPORTER_SANDBOX="${TEST_DIR}" \
PATH="${TEST_DIR}/bin:${PATH}" \
"${JAVA_HOME:+${JAVA_HOME}/bin/}java" \
    -cp "${TEST_DIR}/importer-tests.jar:${JSON_JAR}" \
    cafe.ivory.dejavu.data.PluginPackageImporterTestKt
