#include "dejavu_hook_api.h"

int before_hook(dejavu_hook_context *context) {
    int value = 0;
    DEJAVU_HOOK_TRY(dejavu_hook_get_arg_int(context, 0, &value));
    if (value < 0) {
        return dejavu_hook_return_int(context, 0);
    }
    return dejavu_hook_set_arg_int(context, 0, value + 1);
}

int after_hook(dejavu_hook_context *context) {
    int value = 0;
    DEJAVU_HOOK_TRY(dejavu_hook_get_result_int(context, &value));
    return dejavu_hook_set_result_int(context, value + 1);
}

int compile_all_convenience_helpers(dejavu_hook_context *context) {
    int boolean_value = 0;
    signed char byte_value = 0;
    unsigned short char_value = 0;
    short short_value = 0;
    int int_value = 0;
    long long long_value = 0;
    float float_value = 0;
    double double_value = 0;

    DEJAVU_HOOK_TRY(dejavu_hook_get_arg_boolean(context, 0, &boolean_value));
    DEJAVU_HOOK_TRY(dejavu_hook_get_arg_byte(context, 0, &byte_value));
    DEJAVU_HOOK_TRY(dejavu_hook_get_arg_char(context, 0, &char_value));
    DEJAVU_HOOK_TRY(dejavu_hook_get_arg_short(context, 0, &short_value));
    DEJAVU_HOOK_TRY(dejavu_hook_get_arg_long(context, 0, &long_value));
    DEJAVU_HOOK_TRY(dejavu_hook_get_arg_float(context, 0, &float_value));
    DEJAVU_HOOK_TRY(dejavu_hook_get_arg_double(context, 0, &double_value));
    DEJAVU_HOOK_TRY(dejavu_hook_set_arg_boolean(context, 0, boolean_value));
    DEJAVU_HOOK_TRY(dejavu_hook_set_arg_byte(context, 0, byte_value));
    DEJAVU_HOOK_TRY(dejavu_hook_set_arg_char(context, 0, char_value));
    DEJAVU_HOOK_TRY(dejavu_hook_set_arg_short(context, 0, short_value));
    DEJAVU_HOOK_TRY(dejavu_hook_set_arg_int(context, 0, int_value));
    DEJAVU_HOOK_TRY(dejavu_hook_set_arg_long(context, 0, long_value));
    DEJAVU_HOOK_TRY(dejavu_hook_set_arg_float(context, 0, float_value));
    DEJAVU_HOOK_TRY(dejavu_hook_set_arg_double(context, 0, double_value));
    return DEJAVU_HOOK_OK;
}

int compile_runtime_helpers(dejavu_hook_context *context) {
    char buffer[16];
    size_t size = 0;
    void *object = 0;
    int is_instance = 0;
    dejavu_hook_value argument;
    dejavu_hook_value result;
    argument.kind = DEJAVU_HOOK_VALUE_STRING;
    argument.value.string_value.data = "x";
    argument.value.string_value.size = 1;
    result.kind = DEJAVU_HOOK_VALUE_VOID;
    DEJAVU_HOOK_TRY(dejavu_hook_get_arg_string_mutf8(
        context, 0, buffer, sizeof(buffer), &size));
    DEJAVU_HOOK_TRY(dejavu_hook_set_arg_string_mutf8(context, 0, buffer, size));
    DEJAVU_HOOK_TRY(dejavu_hook_new_string_mutf8(context, buffer, size, &object));
    DEJAVU_HOOK_TRY(dejavu_hook_is_instance_of(
        context, object, "java.lang.String", &is_instance));
    (void)is_instance;
    (void)argument;
    (void)result;
    return DEJAVU_HOOK_OK;
}
