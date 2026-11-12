#!/system/bin/sh
MODDIR=${0%/*}
if sh "$MODDIR/router-control.sh" stop >/dev/null 2>&1; then
    sh "$MODDIR/router-install.sh" remove >/dev/null 2>&1 || true
fi
RUN_DIR="$MODDIR/run"
STOP_FILE="$RUN_DIR/root-helper.stop"
PID_FILE="$RUN_DIR/root-helper.pid"
mkdir -p "$RUN_DIR"
touch "$STOP_FILE"
if [ -f "$PID_FILE" ]; then
    pid=$(cat "$PID_FILE" 2>/dev/null)
    [ -n "$pid" ] && kill -TERM "$pid" 2>/dev/null
fi
pkill -f 'lib/arm64-v8a/dejavu-root-helper' 2>/dev/null || true
pkill -f 'root-watchdog.sh' 2>/dev/null || true
for helper_pid in $(pidof dejavu-root-helper 2>/dev/null); do
    kill -9 "$helper_pid" 2>/dev/null
done
setprop debug.dejavu.root.port ""
setprop debug.dejavu.root.token ""
rm -f "$PID_FILE" "$RUN_DIR/root-helper.log" "$STOP_FILE"
