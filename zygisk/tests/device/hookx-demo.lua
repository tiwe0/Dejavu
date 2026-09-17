if type(hookx) ~= "table" then
    assert(loadfile("/data/adb/modules/dejavu_zygisk/config/init.lua"))()
end

local id = hookx.trace("android.app.Activity", "onResume", "()V")
hook.log("hookx demo installed: " .. id)
hook.log(hook.list())
