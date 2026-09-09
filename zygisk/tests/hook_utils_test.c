#include "dejavu_hook_utils.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct dejavu_hook_context {
    unsigned int phase;
    int arg;
    int result;
    const char *arg_string;
    int instance_of;
    int log_count;
};

unsigned int dejavu_hook_phase(const dejavu_hook_context *context) {
    return context->phase;
}

unsigned long long dejavu_hook_id(const dejavu_hook_context *context) {
    (void)context;
    return 42;
}

unsigned int dejavu_hook_arg_count(const dejavu_hook_context *context) {
    (void)context;
    return 1;
}

void *dejavu_hook_this_object(const dejavu_hook_context *context) {
    (void)context;
    return (void *)0x1234;
}

int dejavu_hook_get_arg_i64(
    dejavu_hook_context *context, unsigned int index, long long *value) {
    if (index != 0 || value == NULL) {
        return DEJAVU_HOOK_ERROR_ARGUMENT;
    }
    *value = context->arg;
    return DEJAVU_HOOK_OK;
}

int dejavu_hook_set_arg_i64(
    dejavu_hook_context *context, unsigned int index, long long value) {
    if (index != 0) {
        return DEJAVU_HOOK_ERROR_ARGUMENT;
    }
    context->arg = (int)value;
    return DEJAVU_HOOK_OK;
}

int dejavu_hook_get_arg_f64(
    dejavu_hook_context *context, unsigned int index, double *value) {
    (void)context;
    (void)index;
    (void)value;
    return DEJAVU_HOOK_ERROR_TYPE;
}

int dejavu_hook_set_arg_f64(
    dejavu_hook_context *context, unsigned int index, double value) {
    (void)context;
    (void)index;
    (void)value;
    return DEJAVU_HOOK_ERROR_TYPE;
}

int dejavu_hook_get_arg_object(
    dejavu_hook_context *context, unsigned int index, void **value) {
    (void)context;
    (void)index;
    (void)value;
    return DEJAVU_HOOK_ERROR_TYPE;
}

int dejavu_hook_set_arg_object(
    dejavu_hook_context *context, unsigned int index, void *value) {
    (void)context;
    (void)index;
    (void)value;
    return DEJAVU_HOOK_ERROR_TYPE;
}

int dejavu_hook_get_result_i64(dejavu_hook_context *context, long long *value) {
    if (value == NULL) {
        return DEJAVU_HOOK_ERROR_ARGUMENT;
    }
    *value = context->result;
    return DEJAVU_HOOK_OK;
}

int dejavu_hook_set_result_i64(dejavu_hook_context *context, long long value) {
    context->result = (int)value;
    return DEJAVU_HOOK_OK;
}

int dejavu_hook_get_result_f64(
    dejavu_hook_context *context, double *value) {
    (void)context;
    (void)value;
    return DEJAVU_HOOK_ERROR_TYPE;
}

int dejavu_hook_set_result_f64(
    dejavu_hook_context *context, double value) {
    (void)context;
    (void)value;
    return DEJAVU_HOOK_ERROR_TYPE;
}

int dejavu_hook_get_result_object(
    dejavu_hook_context *context, void **value) {
    (void)context;
    (void)value;
    return DEJAVU_HOOK_ERROR_TYPE;
}

int dejavu_hook_set_result_object(
    dejavu_hook_context *context, void *value) {
    (void)context;
    (void)value;
    return DEJAVU_HOOK_OK;
}

int dejavu_hook_set_result_void(dejavu_hook_context *context) {
    (void)context;
    return DEJAVU_HOOK_OK;
}

int dejavu_hook_get_arg_string_mutf8(
    dejavu_hook_context *context,
    unsigned int index,
    char *buffer,
    size_t capacity,
    size_t *size) {
    size_t length;
    if (index != 0 || buffer == NULL || size == NULL || context->arg_string == NULL) {
        return DEJAVU_HOOK_ERROR_ARGUMENT;
    }
    length = strlen(context->arg_string);
    *size = length;
    if (capacity == 0 || length >= capacity) {
        return DEJAVU_HOOK_ERROR_RANGE;
    }
    memcpy(buffer, context->arg_string, length + 1);
    return DEJAVU_HOOK_OK;
}

int dejavu_hook_set_arg_string_mutf8(
    dejavu_hook_context *context,
    unsigned int index,
    const char *data,
    size_t size) {
    (void)context;
    (void)index;
    (void)data;
    (void)size;
    return DEJAVU_HOOK_OK;
}

int dejavu_hook_get_result_string_mutf8(
    dejavu_hook_context *context,
    char *buffer,
    size_t capacity,
    size_t *size) {
    (void)context;
    (void)buffer;
    (void)capacity;
    (void)size;
    return DEJAVU_HOOK_ERROR_TYPE;
}

int dejavu_hook_set_result_string_mutf8(
    dejavu_hook_context *context,
    const char *data,
    size_t size) {
    (void)context;
    (void)data;
    (void)size;
    return DEJAVU_HOOK_OK;
}

int dejavu_hook_new_string_mutf8(
    dejavu_hook_context *context,
    const char *data,
    size_t size,
    void **value) {
    (void)context;
    (void)data;
    (void)size;
    if (value == NULL) {
        return DEJAVU_HOOK_ERROR_ARGUMENT;
    }
    *value = (void *)0x5678;
    return DEJAVU_HOOK_OK;
}

int dejavu_hook_is_instance_of(
    dejavu_hook_context *context,
    void *object,
    const char *class_name,
    int *result) {
    (void)object;
    if (class_name == NULL || result == NULL) {
        return DEJAVU_HOOK_ERROR_ARGUMENT;
    }
    *result = context->instance_of && strcmp(class_name, "java.lang.String") == 0;
    return DEJAVU_HOOK_OK;
}

int dejavu_hook_call(
    dejavu_hook_context *context,
    unsigned int flags,
    void *receiver,
    const char *class_name,
    const char *method_name,
    const char *signature,
    const dejavu_hook_value *arguments,
    unsigned int argument_count,
    dejavu_hook_value *result) {
    (void)context; (void)flags; (void)receiver; (void)class_name;
    (void)method_name; (void)signature; (void)arguments;
    (void)argument_count; (void)result;
    return DEJAVU_HOOK_OK;
}

void dejavu_hook_log(const char *message) {
    (void)message;
}

static int read_and_update(dejavu_hook_context *context) {
    int value = 0;
    dj_get_arg_int(context, 0, &value);
    dj_set_arg_int(context, 0, value + 1);
    return value;
}

static int read_string(dejavu_hook_context *context) {
    char buffer[32] = {0};
    return (int)dj_get_arg_string(context, 0, buffer, sizeof(buffer));
}

static int read_result_string(dejavu_hook_context *context) {
    char buffer[8] = {0};
    return (int)dj_get_result_string(context, buffer, sizeof(buffer));
}

static int conditional_return(dejavu_hook_context *context, int value) {
    if (value > 0)
        dj_if_eq(context, value, 1, 99);
    else
        return 7;
    return 0;
}

static int side_effect_return(dejavu_hook_context *context, int *value) {
    dj_return_int(context, (*value)++);
    return -1;
}

static int type_return(dejavu_hook_context *context) {
    dj_if_instanceof(context, dj_get_receiver(context), "java.lang.String", 23);
    return 0;
}

static int instance_probe(dejavu_hook_context *context, int *instance) {
    dj_instanceof(context, (void *)0x1, "java.lang.String", instance);
    return *instance;
}

static int result_probe(dejavu_hook_context *context) {
    int value = 0;
    dj_set_result_int(context, 77);
    dj_get_result_int(context, &value);
    return value;
}

static int try_failure(dejavu_hook_context *context) {
    (void)context;
    dj_try(DEJAVU_HOOK_ERROR_TYPE, "expected failure");
    return 0;
}

static int counter_probe(void) {
    dj_counter(test);
    dj_inc(test);
    dj_add(test, 2);
    return (int)dj_get_count(test);
}

static int sample_probe(void) {
    return dj_sample(3) ? 1 : 0;
}

static int invalid_sample_probe(void) {
    return (dj_sample(0) ? 1 : 0) + (dj_sample(-1) ? 1 : 0);
}

/* Expand every public convenience macro at least once so syntax regressions
 * are caught even when a helper is not exercised by the runtime assertions. */
int compile_all_utils(dejavu_hook_context *context) {
    int integer = 0;
    long long long_value = 0;
    int boolean = 0;
    signed char byte = 0;
    float float_value = 0;
    double double_value = 0;
    void *object = NULL;

    dj_counter(expansion);
    dj_hook_debugf("compile %d", integer);
    dj_get_arg_int(context, 0, &integer);
    dj_get_arg_long(context, 0, &long_value);
    dj_get_arg_bool(context, 0, &boolean);
    dj_get_arg_byte(context, 0, &byte);
    dj_get_arg_float(context, 0, &float_value);
    dj_get_arg_double(context, 0, &double_value);
    dj_get_arg_object(context, 0, &object);
    dj_set_arg_int(context, 0, integer);
    dj_set_arg_long(context, 0, long_value);
    dj_set_arg_bool(context, 0, boolean);
    dj_set_arg_float(context, 0, float_value);
    dj_set_arg_double(context, 0, double_value);
    dj_set_arg_object(context, 0, object);
    dj_set_arg_string(context, 0, "x", 1);
    dj_get_result_int(context, &integer);
    dj_get_result_long(context, &long_value);
    dj_get_result_bool(context, &boolean);
    dj_get_result_float(context, &float_value);
    dj_get_result_double(context, &double_value);
    dj_get_result_object(context, &object);
    (void)dj_get_result_string(context, (char[8]){0}, 8);
    dj_set_result_int(context, integer);
    dj_set_result_long(context, long_value);
    dj_set_result_bool(context, boolean);
    dj_set_result_float(context, float_value);
    dj_set_result_double(context, double_value);
    dj_set_result_object(context, object);
    dj_set_result_string(context, "x", 1);
    dj_set_result_void(context);
    dj_set_result_null(context);
    if (0) {
        dj_return_int(context, 1);
        dj_return_long(context, 1);
        dj_return_bool(context, 1);
        dj_return_null(context);
        dj_return_void(context);
        dj_if_eq(context, integer, 1, 1);
        dj_if_ne(context, integer, 1, 1);
        dj_if_lt(context, integer, 1, 1);
        dj_if_le(context, integer, 1, 1);
        dj_if_gt(context, integer, 1, 1);
        dj_if_ge(context, integer, 1, 1);
        dj_if_null(context, object, 1);
        dj_if_not_null(context, object, 1);
        dj_if_in_range(context, integer, 0, 1, 1);
        dj_if_out_of_range(context, integer, 0, 1, 1);
        dj_if_streq(context, "x", "x", 1);
        dj_if_strne(context, "x", "y", 1);
        dj_if_contains(context, "x", "x", 1);
        dj_if_not_contains(context, "x", "y", 1);
        dj_if_instanceof(context, object, "java.lang.String", 1);
        dj_if_sampled(2) { dj_hook_log("sample"); }
        dj_validate_range(context, 0, 0, 1, 1);
        dj_validate_string(context, 0, 1);
    }
    dj_inc(expansion);
    dj_add(expansion, 2);
    (void)dj_get_count(expansion);
    dj_reset_count(expansion);
    dj_log_count(expansion, "count=%lld");
    dj_try(DEJAVU_HOOK_OK, "ok");
    dj_try_safe(DEJAVU_HOOK_OK, "ok");
    dj_log_all_args(context);
    return 0;
}

int main(void) {
    dejavu_hook_context context = {
        DEJAVU_HOOK_PHASE_BEFORE, 41, 10, "hello", 1, 0
    };
    char buffer[32] = {0};
    int value = 0;
    int instance = 0;

    assert(dj_is_before(&context));
    context.phase = DEJAVU_HOOK_PHASE_AFTER;
    assert(dj_is_after(&context));
    assert(dj_get_hook_id(&context) == 42);
    assert(dj_get_arg_count(&context) == 1);
    assert(dj_get_receiver(&context) == (void *)0x1234);

    context.phase = DEJAVU_HOOK_PHASE_BEFORE;
    assert(read_and_update(&context) == 41);
    assert(context.arg == 42);
    assert(read_string(&context) == 5);
    assert(dj_get_arg_string(&context, 0, buffer, sizeof(buffer)) == 5);
    assert(read_result_string(&context) == 0);
    assert(strcmp(buffer, "hello") == 0);
    assert(instance_probe(&context, &instance) == 1);
    assert(dj_check_type(&context, (void *)0x1, "java.lang.String"));
    assert(type_return(&context) == DEJAVU_HOOK_OK);
    assert(context.result == 23);
    assert(conditional_return(&context, 1) == DEJAVU_HOOK_OK);
    assert(context.result == 99);
    assert(conditional_return(&context, 2) == 0);
    assert(context.result == 99);
    assert(conditional_return(&context, -1) == 7);
    value = 5;
    assert(side_effect_return(&context, &value) == DEJAVU_HOOK_OK);
    assert(value == 6);
    assert(context.result == 5);

    assert(counter_probe() == 3);
    assert(counter_probe() == 6);
    assert(sample_probe() == 1);
    assert(sample_probe() == 0);
    assert(sample_probe() == 0);
    assert(sample_probe() == 1);
    assert(invalid_sample_probe() == 0);

    assert(try_failure(&context) == DEJAVU_HOOK_ERROR_TYPE);
    assert(result_probe(&context) == 77);
    puts("OK: hook utility macros");
    return 0;
}
