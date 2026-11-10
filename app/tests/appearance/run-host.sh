#!/usr/bin/env bash
set -euo pipefail
APP_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
TEST_DIR=$(mktemp -d "${TMPDIR:-/tmp}/dejavu-appearance.XXXXXX")
trap 'rm -rf "${TEST_DIR}"' EXIT
"${KOTLINC:-kotlinc}" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/ManagerAppearance.kt" \
    "${APP_DIR}/tests/appearance/stubs/android/content/Context.kt" \
    "${APP_DIR}/tests/appearance/stubs/androidx/annotation/StringRes.kt" \
    "${APP_DIR}/tests/appearance/R.kt" \
    "${APP_DIR}/tests/appearance/ManagerAppearanceTest.kt" \
    -include-runtime -d "${TEST_DIR}/tests.jar"
"${JAVA_HOME:+${JAVA_HOME}/bin/}java" -jar "${TEST_DIR}/tests.jar"
