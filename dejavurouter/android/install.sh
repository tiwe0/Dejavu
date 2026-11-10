#!/usr/bin/env bash
# Install dejavurouter as a Magisk module and start it immediately.
set -euo pipefail

SERIAL="${ADB_SERIAL:-c44d68aa}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BIN="$ROOT/dejavurouter-android"
MODULE="$HERE/module"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; build Android binary first" >&2
    exit 1
fi
export ADB_SERVER_SOCKET="${ADB_SERVER_SOCKET:-tcp:127.0.0.1:5037}"
ADB="adb -s $SERIAL"

$ADB push "$BIN" /data/local/tmp/dejavurouter-android >/dev/null
$ADB push "$MODULE/module.prop" /data/local/tmp/dejavurouter-module.prop >/dev/null
$ADB push "$MODULE/service.sh" /data/local/tmp/dejavurouter-service.sh >/dev/null
$ADB push "$MODULE/supervisor.sh" /data/local/tmp/dejavurouter-supervisor.sh >/dev/null
$ADB push "$MODULE/post-fs-data.sh" /data/local/tmp/dejavurouter-post-fs-data.sh >/dev/null
$ADB push "$MODULE/config.json" /data/local/tmp/dejavurouter-config.json >/dev/null
$ADB push "$HERE/install-device.sh" /data/local/tmp/dejavurouter-install.sh >/dev/null

$ADB shell "su -c 'sh /data/local/tmp/dejavurouter-install.sh'"