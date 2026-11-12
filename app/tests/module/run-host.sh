#!/usr/bin/env bash
set -euo pipefail
APP_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
JSON_JAR=${JSON_JAR:-"${APP_DIR}/../out/tooling/json-20250517.jar"}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/dejavu-module-tests.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
[[ -f "$JSON_JAR" ]] || { echo 'Set JSON_JAR to the host-test org.json jar' >&2; exit 1; }
"${KOTLINC:-kotlinc}" \
    "$APP_DIR/src/main/java/cafe/ivory/dejavu/data/RootShell.kt" \
    "$APP_DIR/src/main/java/cafe/ivory/dejavu/data/ModuleInstaller.kt" \
    "$APP_DIR/tests/module/ModuleInstallerTest.kt" \
    -classpath "$JSON_JAR" -include-runtime -d "$WORK/module-tests.jar"
"${JAVA_HOME:+${JAVA_HOME}/bin/}java" -cp "$WORK/module-tests.jar:$JSON_JAR" cafe.ivory.dejavu.data.ModuleInstallerTestKt
