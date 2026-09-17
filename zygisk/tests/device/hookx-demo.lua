if type(hookx) ~= "table" then
    local hookx_loader, hookx_error =
        loadfile("/data/adb/modules/dejavu_zygisk/config/hookx.lua")
    if hookx_loader ~= nil then
        assert(hookx_loader)()
    elseif hookx_error ~= nil and
        (hookx_error:find("No such file", 1, true) ~= nil or
            hookx_error:find("cannot open", 1, true) ~= nil) then
        assert(loadfile("/data/adb/modules/dejavu_zygisk/config/init.lua"))()
    else
        error(hookx_error)
    end
end

if type(_G.hookx_demo_id) == "number" then
    hook.uninstall(_G.hookx_demo_id)
end

local id = hookx.trace("android.app.Activity", "onResume", "()V")
_G.hookx_demo_id = id
hook.log("hookx demo installed: " .. id)
hook.log(hook.list())
