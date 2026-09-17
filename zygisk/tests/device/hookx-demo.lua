local id = hookx.trace("android.app.Activity", "onResume", "()V")
hook.log("hookx demo installed: " .. id)
hook.log(hook.list())
