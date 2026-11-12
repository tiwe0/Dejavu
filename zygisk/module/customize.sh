#!/system/bin/sh

ui_print '░▒▓███████▓▒░░▒▓████████▓▒░     ░▒▓█▓▒░░▒▓██████▓▒░░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░'
ui_print '░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░            ░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░'
ui_print '░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░            ░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░'
ui_print '░▒▓█▓▒░░▒▓█▓▒░▒▓██████▓▒░       ░▒▓█▓▒░▒▓████████▓▒░░▒▓█▓▒▒▓█▓▒░░▒▓█▓▒░░▒▓█▓▒░'
ui_print '░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░     ░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░ ░▒▓█▓▓█▓▒░ ░▒▓█▓▒░░▒▓█▓▒░'
ui_print '░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░     ░▒▓█▓▒░░▒▓█▓▒░▒▓█▓▒░░▒▓█▓▒░ ░▒▓█▓▓█▓▒░ ░▒▓█▓▒░░▒▓█▓▒░'
ui_print '░▒▓███████▓▒░░▒▓████████▓▒░▒▓██████▓▒░░▒▓█▓▒░░▒▓█▓▒░  ░▒▓██▓▒░   ░▒▓██████▓▒░'
ui_print ''

if [ "$ARCH" != "arm64" ]; then
    abort "Dejavu Zygisk currently supports arm64 only"
fi

if [ "${API:-0}" -lt 26 ] || [ "${API:-0}" -gt 35 ]; then
    abort "Dejavu Zygisk supports Android 8 through 15 (API 26-35)"
fi

# Keep device-owned scope, trust and startup configuration on module upgrades.
# The assembled init.lua belongs to the new framework and is not copied back.
PREVIOUS=${DEJAVU_PREVIOUS_MODULE_DIR:-/data/adb/modules/dejavu_zygisk}
if [ -d "$PREVIOUS" ] && [ "$PREVIOUS" != "$MODPATH" ]; then
    mkdir -p "$MODPATH/config"
    for relative in config/targets.txt config/plugins.txt system.prop; do
        if [ -f "$PREVIOUS/$relative" ] && [ ! -L "$PREVIOUS/$relative" ]; then
            cp -p "$PREVIOUS/$relative" "$MODPATH/$relative" || abort "Unable to preserve $relative"
        fi
    done
    if [ -d "$PREVIOUS/config/early-hooks" ] && [ ! -L "$PREVIOUS/config/early-hooks" ]; then
        mkdir -p "$MODPATH/config/early-hooks"
        cp -R "$PREVIOUS/config/early-hooks/." "$MODPATH/config/early-hooks/" ||
            abort "Unable to preserve startup hooks"
    fi
fi

set_perm_recursive "$MODPATH" 0 0 0755 0644
mkdir -p "$MODPATH/run"
set_perm "$MODPATH/run" 0 0 0755
set_perm "$MODPATH/zygisk/arm64-v8a.so" 0 0 0755
set_perm "$MODPATH/lib/arm64-v8a/libdejavu_agent.so" 0 0 0755
set_perm "$MODPATH/lib/arm64-v8a/dejavu-root-helper" 0 0 0755
set_perm "$MODPATH/bin/dejavuerouter" 0 0 0755
set_perm "$MODPATH/router-install.sh" 0 0 0755
set_perm "$MODPATH/router-control.sh" 0 0 0755
set_perm "$MODPATH/router-watchdog.sh" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/root-watchdog.sh" 0 0 0755
set_perm "$MODPATH/uninstall.sh" 0 0 0755
chcon -R u:object_r:system_file:s0 "$MODPATH" || abort "Unable to label the module directory"

sh "$MODPATH/router-install.sh" install || abort "Unable to deploy internal Dejavu router"
