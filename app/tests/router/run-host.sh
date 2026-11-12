#!/usr/bin/env bash
set -euo pipefail
APP_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
JSON_JAR=${JSON_JAR:-"${APP_DIR}/../out/tooling/json-20250517.jar"}
TEST_DIR=$(mktemp -d "${TMPDIR:-/tmp}/dejavu-router-tests.XXXXXX")
trap 'rm -rf "${TEST_DIR}"' EXIT
GRADLE_CACHE=${GRADLE_USER_HOME:-"${APP_DIR}/../out/tooling/gradle-home"}/caches
COROUTINES_CANDIDATES=("${GRADLE_CACHE}"/modules-2/files-2.1/org.jetbrains.kotlinx/kotlinx-coroutines-core-jvm/1.10.2/*/*.jar)
VIEWMODEL_CANDIDATES=("${GRADLE_CACHE}"/modules-2/files-2.1/androidx.lifecycle/lifecycle-viewmodel-android/2.9.0/*/*.aar)
COROUTINES_JAR=${COROUTINES_JAR:-${COROUTINES_CANDIDATES[0]}}
VIEWMODEL_AAR=${VIEWMODEL_AAR:-${VIEWMODEL_CANDIDATES[0]}}
[[ -f "$COROUTINES_JAR" && -f "$VIEWMODEL_AAR" ]] || { echo 'Cached coroutines 1.10.2 and lifecycle ViewModel 2.9.0 are required' >&2; exit 1; }
unzip -p "$VIEWMODEL_AAR" classes.jar > "${TEST_DIR}/viewmodel.jar"
CLASSPATH="${JSON_JAR}:${COROUTINES_JAR}:${TEST_DIR}/viewmodel.jar"
"${KOTLINC:-kotlinc}" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/RootShell.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/RouterClient.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/RouterUiState.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/RouterViewModel.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/RpcDeadline.kt" \
    "${APP_DIR}/tests/router/LocalSocket.kt" \
    "${APP_DIR}/tests/router/RouterClientTest.kt" \
    "${APP_DIR}/tests/router/RouterViewModelTest.kt" \
    -classpath "${CLASSPATH}" -include-runtime -d "${TEST_DIR}/router-tests.jar"
"${JAVA_HOME:+${JAVA_HOME}/bin/}java" -cp "${TEST_DIR}/router-tests.jar:${CLASSPATH}" cafe.ivory.dejavu.data.RouterClientTestKt
"${JAVA_HOME:+${JAVA_HOME}/bin/}java" -cp "${TEST_DIR}/router-tests.jar:${CLASSPATH}" cafe.ivory.dejavu.data.RouterViewModelTestKt
