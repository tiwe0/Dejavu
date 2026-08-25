local source = [[
int before_hook(dejavu_hook_context *context) {
    (void)context;
    dejavu_hook_log("MT Activity.onResume native callback");
    return DEJAVU_HOOK_OK;
}
]]

local id = hook.install(
    "android.app.Activity",
    "onResume",
    "()V",
    source)
hook.log("MT Activity.onResume hook installed: " .. id)
hook.log(hook.list())
