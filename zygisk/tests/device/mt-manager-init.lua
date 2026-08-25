local source = [[
extern void *dejavu_hook_call_original(
    void *env, void *backup, void *args, int is_static);
extern void dejavu_hook_log(const char *message);

void *dejavu_hook_callback(
    void *env,
    unsigned long long hook_id,
    void *backup,
    void *args,
    int is_static) {
    (void)hook_id;
    dejavu_hook_log("MT Activity.onResume native callback");
    return dejavu_hook_call_original(env, backup, args, is_static);
}
]]

local id = hook.install(
    "android.app.Activity",
    "onResume",
    "()V",
    source)
hook.log("MT Activity.onResume hook installed: " .. id)
hook.log(hook.list())
