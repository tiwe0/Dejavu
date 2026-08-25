local source = [[
int before_hook(dejavu_hook_context *context) {
    (void)context;
    return DEJAVU_HOOK_OK;
}
]]

local protocol_source = [[
int before_hook(dejavu_hook_context *context) {
    int value = 0;
    DEJAVU_HOOK_TRY(dejavu_hook_get_arg_int(context, 0, &value));
    if (value == 7) {
        return dejavu_hook_set_arg_int(context, 0, 10);
    }
    return dejavu_hook_return_int(context, 100);
}

int after_hook(dejavu_hook_context *context) {
    int result = 0;
    DEJAVU_HOOK_TRY(dejavu_hook_get_result_int(context, &result));
    return dejavu_hook_set_result_int(context, result + 1);
}
]]

local protocol_id = hook.install(
    "io.dejavu.bridge.HookStress",
    "protocolTarget",
    "(I)I",
    protocol_source)
assert(hook.protocol_test(protocol_id))
assert(hook.remove(protocol_id))

local helper_source = [[
int before_hook(dejavu_hook_context *context) {
    char value[64];
    unsigned long size = 0;
    DEJAVU_HOOK_TRY(dejavu_hook_get_arg_string_mutf8(context, 0, value, sizeof(value), &size));
    if (size != 5 || value[0] != 'i' || value[4] != 't') {
        return DEJAVU_HOOK_ERROR_ARGUMENT;
    }
    DEJAVU_HOOK_TRY(dejavu_hook_set_arg_string_mutf8(context, 0, "changed", 7));

    void *created = 0;
    int is_string = 0;
    DEJAVU_HOOK_TRY(dejavu_hook_new_string_mutf8(context, "temporary", 9, &created));
    DEJAVU_HOOK_TRY(dejavu_hook_is_instance_of(context, created, "java.lang.String", &is_string));
    if (!is_string) {
        return DEJAVU_HOOK_ERROR_TYPE;
    }

    dejavu_hook_value argument;
    argument.value.object_value = 0;
    argument.kind = DEJAVU_HOOK_VALUE_I64;
    argument.value.i64_value = 4;
    dejavu_hook_value result;
    result.value.object_value = 0;
    DEJAVU_HOOK_TRY(dejavu_hook_call(
        context,
        DEJAVU_HOOK_CALL_STATIC,
        0,
        "io.dejavu.bridge.HookStress",
        "callTarget",
        "(I)I",
        &argument,
        1,
        &result));
    if (result.kind != DEJAVU_HOOK_VALUE_I64 || result.value.i64_value != 7) {
        return DEJAVU_HOOK_ERROR_TYPE;
    }
    return DEJAVU_HOOK_OK;
}

int after_hook(dejavu_hook_context *context) {
    char value[64];
    unsigned long size = 0;
    DEJAVU_HOOK_TRY(dejavu_hook_get_result_string_mutf8(context, value, sizeof(value), &size));
    if (size != 12 || value[0] != 'o' || value[11] != 'd') {
        return DEJAVU_HOOK_ERROR_ARGUMENT;
    }
    return dejavu_hook_set_result_string_mutf8(context, "after", 5);
}
]]

local helper_id = hook.install(
    "io.dejavu.bridge.HookStress",
    "stringTarget",
    "(Ljava/lang/String;)Ljava/lang/String;",
    helper_source)
assert(hook.helper_test(helper_id))
assert(hook.remove(helper_id))

local object_source = [[
int before_hook(dejavu_hook_context *context) {
    void *value = 0;
    int is_string = 0;
    DEJAVU_HOOK_TRY(dejavu_hook_get_arg_object(context, 0, &value));
    if (value == 0) {
        return DEJAVU_HOOK_OK;
    }
    DEJAVU_HOOK_TRY(dejavu_hook_is_instance_of(context, value, "java.lang.Object", &is_string));
    return is_string ? DEJAVU_HOOK_OK : DEJAVU_HOOK_ERROR_TYPE;
}
]]
local object_id = hook.install(
    "io.dejavu.bridge.HookStress",
    "objectTarget",
    "(Ljava/lang/Object;)Ljava/lang/Object;",
    object_source)
assert(hook.helper_test(object_id))
assert(hook.remove(object_id))

local exception_source = [[
int before_hook(dejavu_hook_context *context) {
    dejavu_hook_value result;
    result.kind = DEJAVU_HOOK_VALUE_VOID;
    result.value.object_value = 0;
    int status = dejavu_hook_call(
        context,
        DEJAVU_HOOK_CALL_STATIC,
        0,
        "io.dejavu.bridge.HookStress",
        "missingExceptionTestMethod",
        "()I",
        0,
        0,
        &result);
    return status == DEJAVU_HOOK_ERROR_JNI
        ? DEJAVU_HOOK_OK
        : DEJAVU_HOOK_ERROR_JNI;
}

int after_hook(dejavu_hook_context *context) {
    return dejavu_hook_set_result_int(context, 999);
}
]]
local exception_id = hook.install(
    "io.dejavu.bridge.HookStress",
    "exceptionTarget",
    "(I)I",
    exception_source)
assert(hook.exception_test(exception_id))
hook.log("exception handling test passed")
assert(hook.remove(exception_id))

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
