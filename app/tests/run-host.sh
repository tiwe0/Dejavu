#!/usr/bin/env bash
set -euo pipefail

APP_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
TEST_DIR=$(mktemp -d "${TMPDIR:-/tmp}/dejavu-console-test.XXXXXX")
trap 'rm -rf "${TEST_DIR}"' EXIT
JSON_JAR=${JSON_JAR:-"${APP_DIR}/../out/tooling/json-20250517.jar"}
if [[ ! -f "${JSON_JAR}" ]]; then
    echo "Missing host-test org.json jar; set JSON_JAR (see app/README.md)." >&2
    exit 1
fi

"${KOTLINC:-kotlinc}" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/ConsoleUiState.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/NativePluginArtifact.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/PluginPackageUiState.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/PluginPackage.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/LuaPackageScripts.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/PackageRuntimeSnapshot.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/PluginEventDecoder.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/PluginEventProjection.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/RpcEndpoint.kt" \
    "${APP_DIR}/src/main/java/cafe/ivory/dejavu/data/RootShell.kt" \
    "${APP_DIR}/tests/stubs/android/graphics/Bitmap.kt" \
    "${APP_DIR}/tests/ConsoleRegressionTest.kt" \
    "${APP_DIR}/tests/PluginPackageRegressionTest.kt" \
    "${APP_DIR}/tests/PluginEventDecoderTest.kt" \
    "${APP_DIR}/tests/PluginEventProjectionTest.kt" \
    -classpath "${JSON_JAR}" \
    -include-runtime -d "${TEST_DIR}/console-tests.jar"
DEJAVU_REPO_ROOT="${APP_DIR}/.." \
DEJAVU_TEST_LUA="${APP_DIR}/../zygisk/out/host-tests/lua" \
"${JAVA_HOME:+${JAVA_HOME}/bin/}java" \
    -cp "${TEST_DIR}/console-tests.jar:${JSON_JAR}" \
    cafe.ivory.dejavu.data.ConsoleRegressionTestKt "$@"
