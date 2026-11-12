#!/system/bin/sh
set -u
umask 077
MODDIR=$1
DATA=$2
BASE=$3
BIN="$BASE/bin/dejavuerouter"
child=
born=
finish() {
    trap '' TERM INT HUP
    executable=
    [ -z "$child" ] || executable=$(readlink "/proc/$child/exe" 2>/dev/null)
    if [ -n "$child" ] && [ "$(awk '{print $22}' "/proc/$child/stat" 2>/dev/null)" = "$born" ] &&
        { [ "$executable" = "$BIN" ] || [ "$executable" = "$BIN (deleted)" ]; }; then
        kill -TERM "$child" 2>/dev/null || true
        wait "$child" 2>/dev/null || true
    fi
    exit 0
}
trap finish TERM INT
trap '' HUP
while [ -f "$DATA/enabled" ] && [ -x "$BIN" ]; do
    rm -f "$DATA/control.json"
    stop_requested=0
    trap 'stop_requested=1' TERM INT
    "$BIN" "$DATA/config.json" &
    child=$!
    born=$(awk '{print $22}' "/proc/$child/stat" 2>/dev/null)
    printf '%s %s\n' "$child" "$born" > "$DATA/daemon.pid.tmp"
    mv "$DATA/daemon.pid.tmp" "$DATA/daemon.pid"
    trap finish TERM INT
    [ "$stop_requested" -eq 0 ] || finish
    wait "$child" 2>/dev/null
    child=
    [ -f "$DATA/enabled" ] || break
    sleep 2
done
