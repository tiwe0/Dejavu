#!/system/bin/sh
# Clear boot-stale supervisor state before late_start.
DATA=/data/adb/dejavurouter
mkdir -p "$DATA"
rm -f "$DATA/supervisor.pid"
rm -rf "$DATA/supervisor.lock"
