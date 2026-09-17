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

set_perm_recursive "$MODPATH" 0 0 0755 0644
mkdir -p "$MODPATH/run"
set_perm "$MODPATH/run" 0 0 0755
set_perm "$MODPATH/zygisk/arm64-v8a.so" 0 0 0755
set_perm "$MODPATH/lib/arm64-v8a/libdejavu_agent.so" 0 0 0755
chcon -R u:object_r:system_file:s0 "$MODPATH" || abort "Unable to label the module directory"
