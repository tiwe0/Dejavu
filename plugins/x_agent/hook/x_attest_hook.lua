-- X attestation r0.b() capture hook.
-- Load this from DejaVu early init only for com.twitter.android.
-- It captures the r0 receiver into com.x.android.utils.d3.a.
local function install_x_attest_hook()
    if _G.__x_attest_hook_installed then
        return
    end
    local source = [[
static int x_attest_set_holder(dejavu_hook_context *context, void *self) {
    dejavu_hook_value argument;
    dejavu_hook_value result;
    void *class_object;
    void *field_object;
    void *reference_object;

    argument.kind = DEJAVU_HOOK_VALUE_STRING;
    argument.value.string_value.data = "com.x.android.utils.d3";
    argument.value.string_value.size = sizeof("com.x.android.utils.d3") - 1;
    if (dejavu_hook_call(context, DEJAVU_HOOK_CALL_STATIC, 0,
                         "java/lang/Class", "forName",
                         "(Ljava/lang/String;)Ljava/lang/Class;",
                         &argument, 1, &result) != DEJAVU_HOOK_OK) {
        return DEJAVU_HOOK_OK;
    }
    class_object = result.value.object_value;
    if (!class_object) {
        return DEJAVU_HOOK_OK;
    }

    argument.value.string_value.data = "a";
    argument.value.string_value.size = 1;
    if (dejavu_hook_call(context, 0, class_object,
                         "java/lang/Class", "getField",
                         "(Ljava/lang/String;)Ljava/lang/reflect/Field;",
                         &argument, 1, &result) != DEJAVU_HOOK_OK) {
        return DEJAVU_HOOK_OK;
    }
    field_object = result.value.object_value;
    if (!field_object) {
        return DEJAVU_HOOK_OK;
    }

    argument.kind = DEJAVU_HOOK_VALUE_OBJECT;
    argument.value.object_value = 0;
    if (dejavu_hook_call(context, 0, field_object,
                         "java/lang/reflect/Field", "get",
                         "(Ljava/lang/Object;)Ljava/lang/Object;",
                         &argument, 1, &result) != DEJAVU_HOOK_OK) {
        return DEJAVU_HOOK_OK;
    }
    reference_object = result.value.object_value;
    if (!reference_object) {
        return DEJAVU_HOOK_OK;
    }

    argument.kind = DEJAVU_HOOK_VALUE_OBJECT;
    argument.value.object_value = self;
    if (dejavu_hook_call(context, 0, reference_object,
                         "java/util/concurrent/atomic/AtomicReference", "set",
                         "(Ljava/lang/Object;)V",
                         &argument, 1, &result) != DEJAVU_HOOK_OK) {
        return DEJAVU_HOOK_OK;
    }
    dejavu_hook_log("X_ATTEST_R0_SAVED");
    return DEJAVU_HOOK_OK;
}

int after_hook(dejavu_hook_context *context) {
    void *self = dejavu_hook_this_object(context);
    if (self) {
        (void)x_attest_set_holder(context, self);
    }
    return DEJAVU_HOOK_OK;
}
]]
    local ok, result = pcall(function()
        return hook.install("com.x.attestation.r0", "b", "()V", source)
    end)
    if ok then
        _G.__x_attest_hook_installed = true
        hook.log("X_ATTEST_R0_HOOK_READY id=" .. tostring(result))
    else
        hook.log("X_ATTEST_R0_HOOK_FAILED " .. tostring(result))
    end
end

install_x_attest_hook()
