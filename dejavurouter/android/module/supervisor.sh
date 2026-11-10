#!/system/bin/sh
# Small supervisor: restart dejavurouter if the process exits.
DATA=/data/adb/dejavurouter
BIN=$DATA/dejavurouter
CFG=$DATA/config.json
LOG=$DATA/router.log
LOCK=$DATA/supervisor.lock
PID=$DATA/supervisor.pid

mkdir -p "$DATA"
if ! mkdir "$LOCK" 2>/dev/null; then
    exit 0
fi
trap 'rmdir "$LOCK" 2>/dev/null' EXIT
echo $$ > "$PID"
trap '' HUP

while true; do
    if [ ! -x "$BIN" ]; then
        echo "$(date) missing $BIN" >> "$DATA/watchdog.log"
        sleep 5
        continue
    fi
    if [ -f "$LOG" ] && [ "$(wc -c < "$LOG")" -gt 5242880 ]; then
        mv -f "$LOG" "$LOG.1"
    fi
    echo "$(date) starting dejavurouter" >> "$DATA/watchdog.log"
    "$BIN" "$CFG" >> "$LOG" 2>&1
    rc=$?
    echo "$(date) dejavurouter exited rc=$rc; restarting" >> "$DATA/watchdog.log"
    sleep 2
done
