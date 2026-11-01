#!/system/bin/sh
# Detached watchdog for the Dejavu root helper.
MODDIR=$1
RUN_DIR="$MODDIR/run"
LOG="$RUN_DIR/root-helper.log"
PID_FILE="$RUN_DIR/root-helper.pid"
STOP_FILE="$RUN_DIR/root-helper.stop"
HELPER="$MODDIR/lib/arm64-v8a/dejavu-root-helper"
TARGETS="$MODDIR/config/targets.txt"

while [ -x "$HELPER" ] && [ ! -f "$STOP_FILE" ]; do
    "$HELPER" --targets "$TARGETS" &
    child=$!
    echo "$child" > "$PID_FILE"
    wait "$child"
    sleep 2
done
