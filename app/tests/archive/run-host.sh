#!/usr/bin/env bash
set -euo pipefail
APP_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
JSON_JAR=${JSON_JAR:-"${APP_DIR}/../out/tooling/json-20250517.jar"}
TEST_DIR=$(mktemp -d "${TMPDIR:-/tmp}/dejavu-archive-test.XXXXXX")
trap 'rm -rf "${TEST_DIR}"' EXIT
"${KOTLINC:-kotlinc}" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/PluginPackage.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/PluginPage.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/PluginZipArchive.kt" \
    "${APP_DIR}/tests/archive/PluginZipArchiveTest.kt" \
    -classpath "${JSON_JAR}" -include-runtime -d "${TEST_DIR}/archive-tests.jar"
"${JAVA_HOME:+${JAVA_HOME}/bin/}java" -Xmx512m -cp "${TEST_DIR}/archive-tests.jar:${JSON_JAR}" \
    cafe.ivory.dejavu.data.PluginZipArchiveTestKt
