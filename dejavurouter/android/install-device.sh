#!/system/bin/sh
set -e
MOD=/data/adb/modules/dejavurouter
DATA=/data/adb/dejavurouter
mkdir -p "$MOD" "$DATA"
if [ -f "$DATA/supervisor.pid" ]; then
    kill "$(cat "$DATA/supervisor.pid")" 2>/dev/null || true
    sleep 1
fi
pkill -x dejavurouter 2>/dev/null || true
rm -f "$DATA/supervisor.pid"
rm -rf "$DATA/supervisor.lock"
cp /data/local/tmp/dejavurouter-android "$DATA/dejavurouter"
cp /data/local/tmp/dejavurouter-module.prop "$MOD/module.prop"
cp /data/local/tmp/dejavurouter-service.sh "$MOD/service.sh"
cp /data/local/tmp/dejavurouter-supervisor.sh "$MOD/supervisor.sh"
cp /data/local/tmp/dejavurouter-post-fs-data.sh "$MOD/post-fs-data.sh"
if [ ! -f "$DATA/config.json" ]; then
    cp /data/local/tmp/dejavurouter-config.json "$DATA/config.json"
fi
chmod 755 "$DATA/dejavurouter" "$MOD/service.sh" "$MOD/supervisor.sh" "$MOD/post-fs-data.sh"
chmod 600 "$DATA/config.json"
sh "$MOD/service.sh"
sleep 1
echo "supervisor_pid=$(cat "$DATA/supervisor.pid" 2>/dev/null)"
pgrep -f /data/adb/dejavurouter/dejavurouter || true