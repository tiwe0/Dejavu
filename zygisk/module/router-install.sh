#!/system/bin/sh
# Deploy the framework's internal router executable outside the module payload.
set -eu
umask 077
MODDIR=$(CDPATH= cd -- "${0%/*}" && pwd)
BASE=${DEJAVU_DIR:-/data/adb/dejavu}
BINDIR="$BASE/bin"
BIN="$BINDIR/dejavuerouter"
SOURCE="$MODDIR/bin/dejavuerouter"
ACTION=${1:-install}
case "$BASE" in /*) ;; *) echo 'Dejavu directory must be absolute' >&2; exit 2;; esac
[ ! -L "$BASE" ] && [ ! -L "$BINDIR" ] || { echo 'Dejavu binary directory must not be a symlink' >&2; exit 1; }
case "$ACTION" in
    remove)
        rm -f "$BIN"
        rmdir "$BINDIR" 2>/dev/null || true
        exit 0
        ;;
    install) ;;
    *) echo 'usage: router-install.sh install|remove' >&2; exit 2;;
esac
[ -f "$SOURCE" ] && [ -x "$SOURCE" ] && [ ! -L "$SOURCE" ] || { echo 'internal router payload missing' >&2; exit 1; }
mkdir -p "$BINDIR"
chmod 0700 "$BINDIR"
if [ -f "$BIN" ] && [ ! -L "$BIN" ] && cmp -s "$SOURCE" "$BIN"; then
    chmod 0700 "$BIN"
    exit 0
fi
temporary=$(mktemp "$BINDIR/.dejavuerouter.XXXXXX")
trap 'rm -f "$temporary"' EXIT
cp "$SOURCE" "$temporary"
chmod 0700 "$temporary"
if [ -x /system/bin/sh ]; then
    chcon u:object_r:system_file:s0 "$temporary"
fi
# Rename preserves a running instance's mappings during a framework upgrade.
mv -f "$temporary" "$BIN"
