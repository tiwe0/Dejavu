#!/usr/bin/env bash
set -euo pipefail
APP_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
JSON_JAR=${JSON_JAR:-"${APP_DIR}/../out/tooling/json-20250517.jar"}
TEST_DIR=$(mktemp -d "${TMPDIR:-/tmp}/dejavu-viewmodel-tests.XXXXXX")
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
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/ShellExecutor.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/PluginStorageSupport.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/RootShell.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/PluginScopeStore.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/EnvironmentViewModel.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/LogsViewModel.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/ManagerRoute.kt" \
    "${APP_DIR}/tests/viewmodel/LogsViewModelTest.kt" \
    "${APP_DIR}/tests/viewmodel/EnvironmentViewModelTest.kt" \
    "${APP_DIR}/tests/viewmodel/ManagerNavigatorTest.kt" \
    -classpath "${CLASSPATH}" -include-runtime -d "${TEST_DIR}/viewmodel-tests.jar"
"${JAVA_HOME:+${JAVA_HOME}/bin/}java" -cp "${TEST_DIR}/viewmodel-tests.jar:${CLASSPATH}" cafe.ivory.dejavu.data.LogsViewModelTestKt
"${JAVA_HOME:+${JAVA_HOME}/bin/}java" -cp "${TEST_DIR}/viewmodel-tests.jar:${CLASSPATH}" cafe.ivory.dejavu.data.EnvironmentViewModelTestKt
"${JAVA_HOME:+${JAVA_HOME}/bin/}java" -cp "${TEST_DIR}/viewmodel-tests.jar:${CLASSPATH}" cafe.ivory.dejavu.data.ManagerNavigatorTestKt
