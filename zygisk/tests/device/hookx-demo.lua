if type(hookx) ~= "table" then
    local hookx_loader = loadfile("/data/adb/modules/dejavu_zygisk/config/hookx.lua")
    if hookx_loader ~= nil then
        assert(hookx_loader)()
    else
        assert(loadfile("/data/adb/modules/dejavu_zygisk/config/init.lua"))()
    end
end

local id = hookx.trace("android.app.Activity", "onResume", "()V")
hook.log("hookx demo installed: " .. id)
hook.log(hook.list())
