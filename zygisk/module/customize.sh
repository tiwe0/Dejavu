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

if [ "${MAGISK_VER_CODE:-0}" -lt 27000 ]; then
    abort "Dejavu Zygisk requires Magisk 27.0 or newer"
fi

if [ "${API:-0}" -lt 35 ]; then
    abort "Dejavu Zygisk requires Android 15 (API 35) or newer"
fi

set_perm_recursive "$MODPATH" 0 0 0755 0644
mkdir -p "$MODPATH/run"
set_perm "$MODPATH/run" 0 0 0755
set_perm "$MODPATH/zygisk/arm64-v8a.so" 0 0 0755
set_perm "$MODPATH/lib/arm64-v8a/libdejavu_agent.so" 0 0 0755
chcon -R u:object_r:system_file:s0 "$MODPATH" || abort "Unable to label the module directory"
