do
    local f = io.open("/proc/self/cmdline", "rb")
    local cmdline = f and f:read("*a") or ""
    if f then f:close() end
    if string.find(cmdline, "com.twitter.android", 1, true) then
        local ok, err = pcall(function()
            local hook_file = loadfile("/data/adb/modules/dejavu_zygisk/config/x_attest_hook.lua")
            if not hook_file then
                error("cannot load x_attest_hook.lua")
            end
            hook_file()
        end)
        if not ok then
            hook.log("X_ATTEST_R0_INIT_FAILED " .. tostring(err))
        end
    end
end
