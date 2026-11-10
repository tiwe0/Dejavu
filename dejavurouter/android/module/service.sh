#!/system/bin/sh
# Magisk late_start service: start the dejavurouter supervisor.
MODDIR=${0%/*}
DATA=/data/adb/dejavurouter
PIDFILE=$DATA/supervisor.pid

mkdir -p "$DATA"
if [ -f "$PIDFILE" ]; then
    pid=$(cat "$PIDFILE" 2>/dev/null)
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        exit 0
    fi
fi
rm -rf "$DATA/supervisor.lock"
nohup sh "$MODDIR/supervisor.sh" >> "$DATA/watchdog.log" 2>&1 &
echo $! > "$PIDFILE"
