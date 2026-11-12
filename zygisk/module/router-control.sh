#!/system/bin/sh
# Root-only lifecycle entry. The manager is a client, never the daemon's parent.
set -u
umask 077
MODDIR=$(CDPATH= cd -- "${0%/*}" && pwd)
BASE=${DEJAVU_DIR:-/data/adb/dejavu}
DATA=${DEJAVU_ROUTER_DATA_DIR:-$BASE/router}
BIN="$BASE/bin/dejavuerouter"
WATCHDOG="$MODDIR/router-watchdog.sh"
SHELL_BIN=${DEJAVU_ROUTER_SHELL:-/system/bin/sh}
LOCK="$DATA/lifecycle.lock"
ACTION=${1:-status}
case "$BASE" in /*) ;; *) echo 'Dejavu directory must be absolute' >&2; exit 2;; esac
case "$DATA" in /*) ;; *) echo 'router data directory must be absolute' >&2; exit 2;; esac
[ ! -L "$DATA" ] || { echo 'router data directory must not be a symlink' >&2; exit 1; }
mkdir -p "$DATA" || exit 1
chmod 0700 "$DATA" || exit 1

# Start time is checked as well as PID, so stale files cannot signal a reused PID.
start_time() { awk '{print $22}' "/proc/$1/stat" 2>/dev/null; }
alive() {
    record=$1
    [ -f "$record" ] || return 1
    read -r pid born < "$record" || return 1
    case "$pid" in ''|*[!0-9]*) return 1;; esac
    case "$born" in ''|*[!0-9]*) return 1;; esac
    [ "$(start_time "$pid")" = "$born" ] || return 1
    [ "$(awk '{print $3}' "/proc/$pid/stat" 2>/dev/null)" != Z ] || return 1
    kill -0 "$pid" 2>/dev/null
}
daemon_alive() {
    alive "$DATA/daemon.pid" || return 1
    executable=$(readlink "/proc/$pid/exe" 2>/dev/null)
    [ "$executable" = "$BIN" ] || [ "$executable" = "$BIN (deleted)" ]
}
watchdog_alive() {
    alive "$DATA/watchdog.pid" || return 1
    command=$(tr '\000' ' ' < "/proc/$pid/cmdline" 2>/dev/null)
    case "$command" in *"$WATCHDOG $MODDIR $DATA $BASE ") return 0;; *) return 1;; esac
}
status() {
    running=false; current_pid=0; enabled=false
    if daemon_alive; then running=true; current_pid=$pid; fi
    [ ! -f "$DATA/enabled" ] || enabled=true
    installed=false
    [ ! -x "$BIN" ] || installed=true
    printf '{"installed":%s,"running":%s,"enabled":%s,"pid":%s}\n' "$installed" "$running" "$enabled" "$current_pid"
}
[ "$ACTION" != status ] || { status; exit 0; }
case "$ACTION" in start|stop|boot) ;; *) echo 'usage: router-control.sh status|start|stop|boot' >&2; exit 2;; esac
# Kernel-owned locks are released if the controller crashes. Do not unlink the
# lock file: its stable inode serializes concurrent manager and boot requests.
command -v flock >/dev/null 2>&1 || { echo 'flock is required for router lifecycle' >&2; exit 1; }
exec 9> "$LOCK" || exit 1
i=0
until flock -n 9 9>&9 2>/dev/null; do
    i=$((i + 1)); [ "$i" -lt 60 ] || { echo 'router lifecycle busy' >&2; exit 1; }
    sleep 0.1
done

stop_daemon() {
    rm -f "$DATA/enabled"
    if watchdog_alive; then kill -TERM "$pid" 2>/dev/null || true; fi
    if daemon_alive; then kill -TERM "$pid" 2>/dev/null || true; fi
    i=0
    while daemon_alive || watchdog_alive; do
        i=$((i + 1)); [ "$i" -lt 30 ] || break
        sleep 0.1
    done
    if watchdog_alive; then kill -KILL "$pid" 2>/dev/null || true; fi
    if daemon_alive; then kill -KILL "$pid" 2>/dev/null || true; fi
    i=0
    while daemon_alive || watchdog_alive; do
        i=$((i + 1)); [ "$i" -lt 20 ] || { echo 'router did not stop' >&2; return 1; }
        sleep 0.1
    done
    rm -f "$DATA/daemon.pid" "$DATA/watchdog.pid" "$DATA/control.json"
}
if [ "$ACTION" = stop ]; then stop_daemon || exit 1; status; exit 0; fi
if [ "$ACTION" = boot ] && [ ! -f "$DATA/enabled" ]; then status; exit 0; fi
[ ! -f "$MODDIR/disable" ] && [ ! -f "$MODDIR/remove" ] || { echo 'router module is disabled or pending removal' >&2; exit 1; }
[ -x "$BIN" ] && [ -f "$WATCHDOG" ] && [ -x "$SHELL_BIN" ] || { echo 'router installation incomplete' >&2; exit 1; }
if [ ! -f "$DATA/config.json" ]; then
    cp "$MODDIR/router-config.example.json" "$DATA/config.json" || exit 1
fi
[ -f "$DATA/wake_targets.json" ] || printf '{}\n' > "$DATA/wake_targets.json"
chmod 0600 "$DATA/config.json" "$DATA/wake_targets.json"
touch "$DATA/enabled"
if daemon_alive && watchdog_alive && [ -f "$DATA/control.json" ]; then status; exit 0; fi
# Repair an orphan using its verified identity, then launch a fresh supervisor.
stop_daemon || exit 1
touch "$DATA/enabled"
setsid "$SHELL_BIN" "$WATCHDOG" "$MODDIR" "$DATA" "$BASE" </dev/null >/dev/null 2>&1 9>&- &
watchdog_pid=$!
printf '%s %s\n' "$watchdog_pid" "$(start_time "$watchdog_pid")" > "$DATA/watchdog.pid"
i=0
while [ "$i" -lt 50 ]; do
    if daemon_alive && [ -s "$DATA/control.json" ]; then status; exit 0; fi
    if ! watchdog_alive; then break; fi
    i=$((i + 1)); sleep 0.1
done
stop_daemon || true
echo 'router failed to start (configuration, listener or daemon initialization)' >&2
exit 1
