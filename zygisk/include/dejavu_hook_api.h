#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEJAVU_HOOK_ABI_VERSION 1

typedef struct dejavu_hook_context dejavu_hook_context;

enum dejavu_hook_status {
    DEJAVU_HOOK_OK = 0,
    DEJAVU_HOOK_ERROR_ARGUMENT = -1,
    DEJAVU_HOOK_ERROR_PHASE = -2,
    DEJAVU_HOOK_ERROR_TYPE = -3,
    DEJAVU_HOOK_ERROR_RANGE = -4,
    DEJAVU_HOOK_ERROR_JNI = -5,
};

enum dejavu_hook_phase {
    DEJAVU_HOOK_PHASE_BEFORE = 1,
    DEJAVU_HOOK_PHASE_AFTER = 2,
};

enum dejavu_hook_value_kind {
    DEJAVU_HOOK_VALUE_VOID = 0,
    DEJAVU_HOOK_VALUE_BOOLEAN = 1,
    DEJAVU_HOOK_VALUE_I64 = 2,
    DEJAVU_HOOK_VALUE_F64 = 3,
    DEJAVU_HOOK_VALUE_OBJECT = 4,
    DEJAVU_HOOK_VALUE_STRING = 5,
};

enum dejavu_hook_call_flags {
    DEJAVU_HOOK_CALL_STATIC = 1,
};

typedef struct dejavu_hook_string {
    const char *data;
    size_t size;
} dejavu_hook_string;

typedef struct dejavu_hook_value {
    unsigned int kind;
    union {
        int boolean_value;
        long long i64_value;
        double f64_value;
        void *object_value;
        dejavu_hook_string string_value;
    } value;
} dejavu_hook_value;

unsigned int dejavu_hook_abi_version(void);
unsigned int dejavu_hook_phase(const dejavu_hook_context *context);
unsigned long long dejavu_hook_id(const dejavu_hook_context *context);
unsigned int dejavu_hook_arg_count(const dejavu_hook_context *context);
void *dejavu_hook_this_object(const dejavu_hook_context *context);

int dejavu_hook_get_arg_i64(
    dejavu_hook_context *context, unsigned int index, long long *value);
int dejavu_hook_set_arg_i64(
    dejavu_hook_context *context, unsigned int index, long long value);
int dejavu_hook_get_arg_f64(
    dejavu_hook_context *context, unsigned int index, double *value);
int dejavu_hook_set_arg_f64(
    dejavu_hook_context *context, unsigned int index, double value);
int dejavu_hook_get_arg_object(
    dejavu_hook_context *context, unsigned int index, void **value);
int dejavu_hook_set_arg_object(
    dejavu_hook_context *context, unsigned int index, void *value);

int dejavu_hook_get_result_i64(dejavu_hook_context *context, long long *value);
int dejavu_hook_set_result_i64(dejavu_hook_context *context, long long value);
int dejavu_hook_get_result_f64(dejavu_hook_context *context, double *value);
int dejavu_hook_set_result_f64(dejavu_hook_context *context, double value);
int dejavu_hook_get_result_object(dejavu_hook_context *context, void **value);
int dejavu_hook_set_result_object(dejavu_hook_context *context, void *value);
int dejavu_hook_set_result_void(dejavu_hook_context *context);

int dejavu_hook_get_arg_string_mutf8(
    dejavu_hook_context *context,
    unsigned int index,
    char *buffer,
    size_t capacity,
    size_t *size);
int dejavu_hook_set_arg_string_mutf8(
    dejavu_hook_context *context,
    unsigned int index,
    const char *data,
    size_t size);
int dejavu_hook_get_result_string_mutf8(
    dejavu_hook_context *context,
    char *buffer,
    size_t capacity,
    size_t *size);
int dejavu_hook_set_result_string_mutf8(
    dejavu_hook_context *context,
    const char *data,
    size_t size);
int dejavu_hook_new_string_mutf8(
    dejavu_hook_context *context,
    const char *data,
    size_t size,
    void **value);
int dejavu_hook_is_instance_of(
    dejavu_hook_context *context,
    void *object,
    const char *class_name,
    int *result);
int dejavu_hook_call(
    dejavu_hook_context *context,
    unsigned int flags,
    void *receiver,
    const char *class_name,
    const char *method_name,
    const char *signature,
    const dejavu_hook_value *arguments,
    unsigned int argument_count,
    dejavu_hook_value *result);

#define DEJAVU_HOOK_TRY(expression)                  \
    do {                                             \
        int dejavu_hook_status__ = (expression);     \
        if (dejavu_hook_status__ != DEJAVU_HOOK_OK)  \
            return dejavu_hook_status__;             \
    } while (0)

#define DEJAVU_HOOK_DEFINE_I64_TYPE(name, type)                                  \
    static inline int dejavu_hook_get_arg_##name(                                \
        dejavu_hook_context *context, unsigned int index, type *value) {          \
        long long raw = 0;                                                        \
        int status = dejavu_hook_get_arg_i64(context, index, value ? &raw : 0);  \
        if (status == DEJAVU_HOOK_OK)                                             \
            *value = (type)raw;                                                   \
        return status;                                                            \
    }                                                                             \
    static inline int dejavu_hook_set_arg_##name(                                 \
        dejavu_hook_context *context, unsigned int index, type value) {           \
        return dejavu_hook_set_arg_i64(context, index, (long long)value);         \
    }                                                                             \
    static inline int dejavu_hook_get_result_##name(                              \
        dejavu_hook_context *context, type *value) {                              \
        long long raw = 0;                                                        \
        int status = dejavu_hook_get_result_i64(context, value ? &raw : 0);       \
        if (status == DEJAVU_HOOK_OK)                                             \
            *value = (type)raw;                                                   \
        return status;                                                            \
    }                                                                             \
    static inline int dejavu_hook_set_result_##name(                              \
        dejavu_hook_context *context, type value) {                               \
        return dejavu_hook_set_result_i64(context, (long long)value);             \
    }                                                                             \
    static inline int dejavu_hook_return_##name(                                  \
        dejavu_hook_context *context, type value) {                               \
        return dejavu_hook_set_result_##name(context, value);                     \
    }

#define DEJAVU_HOOK_DEFINE_F64_TYPE(name, type)                                   \
    static inline int dejavu_hook_get_arg_##name(                                 \
        dejavu_hook_context *context, unsigned int index, type *value) {           \
        double raw = 0;                                                           \
        int status = dejavu_hook_get_arg_f64(context, index, value ? &raw : 0);   \
        if (status == DEJAVU_HOOK_OK)                                             \
            *value = (type)raw;                                                   \
        return status;                                                            \
    }                                                                             \
    static inline int dejavu_hook_set_arg_##name(                                 \
        dejavu_hook_context *context, unsigned int index, type value) {            \
        return dejavu_hook_set_arg_f64(context, index, (double)value);            \
    }                                                                             \
    static inline int dejavu_hook_get_result_##name(                              \
        dejavu_hook_context *context, type *value) {                              \
        double raw = 0;                                                           \
        int status = dejavu_hook_get_result_f64(context, value ? &raw : 0);       \
        if (status == DEJAVU_HOOK_OK)                                             \
            *value = (type)raw;                                                   \
        return status;                                                            \
    }                                                                             \
    static inline int dejavu_hook_set_result_##name(                              \
        dejavu_hook_context *context, type value) {                               \
        return dejavu_hook_set_result_f64(context, (double)value);                \
    }                                                                             \
    static inline int dejavu_hook_return_##name(                                  \
        dejavu_hook_context *context, type value) {                               \
        return dejavu_hook_set_result_##name(context, value);                     \
    }

DEJAVU_HOOK_DEFINE_I64_TYPE(boolean, int)
DEJAVU_HOOK_DEFINE_I64_TYPE(byte, signed char)
DEJAVU_HOOK_DEFINE_I64_TYPE(char, unsigned short)
DEJAVU_HOOK_DEFINE_I64_TYPE(short, short)
DEJAVU_HOOK_DEFINE_I64_TYPE(int, int)
DEJAVU_HOOK_DEFINE_I64_TYPE(long, long long)
DEJAVU_HOOK_DEFINE_F64_TYPE(float, float)
DEJAVU_HOOK_DEFINE_F64_TYPE(double, double)

#undef DEJAVU_HOOK_DEFINE_I64_TYPE
#undef DEJAVU_HOOK_DEFINE_F64_TYPE

static inline int dejavu_hook_return_object(
    dejavu_hook_context *context, void *value) {
    return dejavu_hook_set_result_object(context, value);
}

static inline int dejavu_hook_return_null(dejavu_hook_context *context) {
    return dejavu_hook_set_result_object(context, 0);
}

static inline int dejavu_hook_return_void(dejavu_hook_context *context) {
    return dejavu_hook_set_result_void(context);
}

void dejavu_hook_log(const char *message);

/* User source may define either callback. Zero means success. */
int before_hook(dejavu_hook_context *context);
int after_hook(dejavu_hook_context *context);

#ifdef __cplusplus
}
#endif
