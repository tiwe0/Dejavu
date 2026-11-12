#!/system/bin/sh
# Root helper entry for Dejavu. Runs as root from Magisk late_start service.
MODDIR=${0%/*}
# Resume only a router explicitly enabled by its owner.
if sh "$MODDIR/router-install.sh" install >/dev/null 2>&1; then
    sh "$MODDIR/router-control.sh" boot >/dev/null 2>&1 || true
fi
RUN_DIR="$MODDIR/run"
LOG="$RUN_DIR/root-helper.log"
PID_FILE="$RUN_DIR/root-helper.pid"
STOP_FILE="$RUN_DIR/root-helper.stop"
WATCHDOG="$MODDIR/root-watchdog.sh"
HELPER="$MODDIR/lib/arm64-v8a/dejavu-root-helper"

mkdir -p "$RUN_DIR"
rm -f "$STOP_FILE"
if [ -f "$PID_FILE" ]; then
    old_pid=$(cat "$PID_FILE" 2>/dev/null)
    [ -n "$old_pid" ] && kill -9 "$old_pid" 2>/dev/null
fi
pkill -f 'lib/arm64-v8a/dejavu-root-helper' 2>/dev/null || true
pkill -f 'root-watchdog.sh' 2>/dev/null || true
for helper_pid in $(pidof dejavu-root-helper 2>/dev/null); do
    kill -9 "$helper_pid" 2>/dev/null
done
sleep 1
rm -f "$PID_FILE"
chmod 0700 "$RUN_DIR"
chmod 0600 "$LOG" 2>/dev/null || true

if [ ! -x "$HELPER" ] || [ ! -f "$WATCHDOG" ]; then
    echo "Dejavu root helper/watchdog missing" >&2
    exit 0
fi

setsid /system/bin/sh "$WATCHDOG" "$MODDIR" </dev/null >>"$LOG" 2>&1 &
echo $! > "$PID_FILE"
chmod 0600 "$PID_FILE"
