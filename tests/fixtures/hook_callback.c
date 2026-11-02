/* Freestanding callback compiled against the runtime's actual Hook ABI. */
int after_hook(dejavu_hook_context *context) {
    long long value = 0;
    void *object = 0;
    if (dejavu_hook_get_result_i64(context, &value) != DEJAVU_HOOK_OK)
        return DEJAVU_HOOK_OK;
    if (value >= 0)
        return dejavu_hook_get_arg_object(context, 0, &object);
    return DEJAVU_HOOK_OK;
}
