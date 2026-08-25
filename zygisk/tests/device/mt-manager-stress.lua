local source = [[
extern void *dejavu_hook_call_original(
    void *env, void *backup, void *args, int is_static);

void *dejavu_hook_callback(
    void *env,
    unsigned long long hook_id,
    void *backup,
    void *args,
    int is_static) {
    (void)hook_id;
    return dejavu_hook_call_original(env, backup, args, is_static);
}
]]

local stress_id = hook.install(
    "io.dejavu.bridge.HookStress",
    "stressTarget",
    "(I)I",
    source)
assert(hook.stress(stress_id, 16, 10000, true))
assert(hook.remove(stress_id))
assert(not hook.remove(stress_id))
assert(hook.stress(stress_id, 8, 2000, false))
hook.log("remove state:\n" .. hook.list())

local clear_id = hook.install(
    "android.app.Activity",
    "onResume",
    "()V",
    source)
assert(clear_id > stress_id)
assert(hook.clear() == 1)
hook.log("clear state:\n" .. hook.list())
