#include "hook_api_runtime_internal.h"

#include <android/log.h>

#include <string>

namespace dejavu_hook_internal {

constexpr char kLogTag[] = "DejavuHook";
constexpr char kHookApiPreamble[] = R"DEJAVU_HOOK_API(
#define DEJAVU_HOOK_ABI_VERSION 1
typedef struct dejavu_hook_context dejavu_hook_context;
enum dejavu_hook_status {
    DEJAVU_HOOK_OK = 0,
    DEJAVU_HOOK_ERROR_ARGUMENT = -1,
    DEJAVU_HOOK_ERROR_PHASE = -2,
    DEJAVU_HOOK_ERROR_TYPE = -3,
    DEJAVU_HOOK_ERROR_RANGE = -4,
    DEJAVU_HOOK_ERROR_JNI = -5
};
enum dejavu_hook_phase {
    DEJAVU_HOOK_PHASE_BEFORE = 1,
    DEJAVU_HOOK_PHASE_AFTER = 2
};
enum dejavu_hook_value_kind {
    DEJAVU_HOOK_VALUE_VOID = 0,
    DEJAVU_HOOK_VALUE_BOOLEAN = 1,
    DEJAVU_HOOK_VALUE_I64 = 2,
    DEJAVU_HOOK_VALUE_F64 = 3,
    DEJAVU_HOOK_VALUE_OBJECT = 4,
    DEJAVU_HOOK_VALUE_STRING = 5
};
enum dejavu_hook_call_flags {
    DEJAVU_HOOK_CALL_STATIC = 1
};
extern unsigned int dejavu_hook_abi_version(void);
extern unsigned int dejavu_hook_phase(const dejavu_hook_context *context);
extern unsigned long long dejavu_hook_id(const dejavu_hook_context *context);
extern unsigned int dejavu_hook_arg_count(const dejavu_hook_context *context);
extern void *dejavu_hook_this_object(const dejavu_hook_context *context);
extern int dejavu_hook_get_arg_i64(dejavu_hook_context *, unsigned int, long long *);
extern int dejavu_hook_set_arg_i64(dejavu_hook_context *, unsigned int, long long);
extern int dejavu_hook_get_arg_f64(dejavu_hook_context *, unsigned int, double *);
extern int dejavu_hook_set_arg_f64(dejavu_hook_context *, unsigned int, double);
extern int dejavu_hook_get_arg_object(dejavu_hook_context *, unsigned int, void **);
extern int dejavu_hook_set_arg_object(dejavu_hook_context *, unsigned int, void *);
extern int dejavu_hook_get_result_i64(dejavu_hook_context *, long long *);
extern int dejavu_hook_set_result_i64(dejavu_hook_context *, long long);
extern int dejavu_hook_get_result_f64(dejavu_hook_context *, double *);
extern int dejavu_hook_set_result_f64(dejavu_hook_context *, double);
extern int dejavu_hook_get_result_object(dejavu_hook_context *, void **);
extern int dejavu_hook_set_result_object(dejavu_hook_context *, void *);
extern int dejavu_hook_set_result_void(dejavu_hook_context *);
typedef struct dejavu_hook_string {
    const char *data;
    unsigned long size;
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
extern int dejavu_hook_get_arg_string_mutf8(
    dejavu_hook_context *, unsigned int, char *, unsigned long, unsigned long *);
extern int dejavu_hook_set_arg_string_mutf8(
    dejavu_hook_context *, unsigned int, const char *, unsigned long);
extern int dejavu_hook_get_result_string_mutf8(
    dejavu_hook_context *, char *, unsigned long, unsigned long *);
extern int dejavu_hook_set_result_string_mutf8(
    dejavu_hook_context *, const char *, unsigned long);
extern int dejavu_hook_new_string_mutf8(
    dejavu_hook_context *, const char *, unsigned long, void **);
extern int dejavu_hook_is_instance_of(
    dejavu_hook_context *, void *, const char *, int *);
extern int dejavu_hook_call(
    dejavu_hook_context *, unsigned int, void *, const char *, const char *, const char *,
    const dejavu_hook_value *, unsigned int, dejavu_hook_value *);
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
extern void dejavu_hook_log(const char *message);
#line 1 "hook.c"
)DEJAVU_HOOK_API";

void set_error(std::string *error, const char *message) {
    if (error != nullptr) {
        error->assign(message != nullptr ? message : "unknown error");
    }
}


int mark_context_error(dejavu_hook_context *context, int status) {
    if (context != nullptr && context->helper_status == DEJAVU_HOOK_OK) {
        context->helper_status = status;
    }
    return status;
}

int check_context(dejavu_hook_context *context) {
    if (context == nullptr || context->env == nullptr || context->signature == nullptr ||
        context->jni == nullptr) {
        return mark_context_error(context, DEJAVU_HOOK_ERROR_ARGUMENT);
    }
    return DEJAVU_HOOK_OK;
}

int check_argument(
    dejavu_hook_context *context,
    unsigned int index,
    char *kind,
    jobject *boxed) {
    int status = check_context(context);
    if (status != DEJAVU_HOOK_OK || index >= context->signature->parameters.size() ||
        context->arguments == nullptr) {
        return mark_context_error(context, DEJAVU_HOOK_ERROR_ARGUMENT);
    }
    *kind = context->signature->parameters[index];
    *boxed = context->env->GetObjectArrayElement(
        context->arguments,
        static_cast<jsize>(context->argument_offset + index));
    if (context->env->ExceptionCheck()) {
        context->env->ExceptionClear();
        return mark_context_error(context, DEJAVU_HOOK_ERROR_JNI);
    }
    return DEJAVU_HOOK_OK;
}


void clear_local(JNIEnv *env, jobject value) {
    if (env != nullptr && value != nullptr) {
        env->DeleteLocalRef(value);
    }
}




}  // namespace dejavu_hook_internal

using namespace dejavu_hook_internal;

const char *dejavu_hook_api_preamble() {
    return kHookApiPreamble;
}

bool dejavu_hook_api_initialize(
    JNIEnv *env,
    DejavuHookJniApi *api,
    std::string *error) {
    struct Descriptor {
        const char *class_name;
        const char *unbox_name;
        const char *unbox_signature;
        const char *box_signature;
    };
    const Descriptor descriptors[] = {
        {"java/lang/Boolean", "booleanValue", "()Z", "(Z)Ljava/lang/Boolean;"},
        {"java/lang/Byte", "byteValue", "()B", "(B)Ljava/lang/Byte;"},
        {"java/lang/Character", "charValue", "()C", "(C)Ljava/lang/Character;"},
        {"java/lang/Short", "shortValue", "()S", "(S)Ljava/lang/Short;"},
        {"java/lang/Integer", "intValue", "()I", "(I)Ljava/lang/Integer;"},
        {"java/lang/Long", "longValue", "()J", "(J)Ljava/lang/Long;"},
        {"java/lang/Float", "floatValue", "()F", "(F)Ljava/lang/Float;"},
        {"java/lang/Double", "doubleValue", "()D", "(D)Ljava/lang/Double;"},
    };
    for (size_t index = 0; index < std::size(descriptors); ++index) {
        jclass local = env->FindClass(descriptors[index].class_name);
        api->primitives[index].type =
            local == nullptr ? nullptr : static_cast<jclass>(env->NewGlobalRef(local));
        api->primitives[index].unbox = local == nullptr
            ? nullptr
            : env->GetMethodID(
                  local, descriptors[index].unbox_name, descriptors[index].unbox_signature);
        api->primitives[index].box = local == nullptr
            ? nullptr
            : env->GetStaticMethodID(local, "valueOf", descriptors[index].box_signature);
        if (env->ExceptionCheck() || api->primitives[index].type == nullptr ||
            api->primitives[index].unbox == nullptr || api->primitives[index].box == nullptr) {
            env->ExceptionClear();
            set_error(error, "unable to initialize primitive hook value API");
            return false;
        }
    }
    return true;
}

bool dejavu_hook_api_register_symbols(
    DejavuRuntime *runtime,
    dejavu_engine *engine,
    std::string *error) {
    if (runtime == nullptr || engine == nullptr) {
        set_error(error, "invalid hook API symbol registration argument");
        return false;
    }
    struct HookSymbol {
        const char *name;
        const void *address;
    };
    const HookSymbol symbols[] = {
        {"dejavu_hook_abi_version", reinterpret_cast<const void *>(dejavu_hook_abi_version)},
        {"dejavu_hook_phase", reinterpret_cast<const void *>(dejavu_hook_phase)},
        {"dejavu_hook_id", reinterpret_cast<const void *>(dejavu_hook_id)},
        {"dejavu_hook_arg_count", reinterpret_cast<const void *>(dejavu_hook_arg_count)},
        {"dejavu_hook_this_object", reinterpret_cast<const void *>(dejavu_hook_this_object)},
        {"dejavu_hook_get_arg_i64", reinterpret_cast<const void *>(dejavu_hook_get_arg_i64)},
        {"dejavu_hook_set_arg_i64", reinterpret_cast<const void *>(dejavu_hook_set_arg_i64)},
        {"dejavu_hook_get_arg_f64", reinterpret_cast<const void *>(dejavu_hook_get_arg_f64)},
        {"dejavu_hook_set_arg_f64", reinterpret_cast<const void *>(dejavu_hook_set_arg_f64)},
        {"dejavu_hook_get_arg_object", reinterpret_cast<const void *>(dejavu_hook_get_arg_object)},
        {"dejavu_hook_set_arg_object", reinterpret_cast<const void *>(dejavu_hook_set_arg_object)},
        {"dejavu_hook_get_result_i64", reinterpret_cast<const void *>(dejavu_hook_get_result_i64)},
        {"dejavu_hook_set_result_i64", reinterpret_cast<const void *>(dejavu_hook_set_result_i64)},
        {"dejavu_hook_get_result_f64", reinterpret_cast<const void *>(dejavu_hook_get_result_f64)},
        {"dejavu_hook_set_result_f64", reinterpret_cast<const void *>(dejavu_hook_set_result_f64)},
        {"dejavu_hook_get_result_object", reinterpret_cast<const void *>(dejavu_hook_get_result_object)},
        {"dejavu_hook_set_result_object", reinterpret_cast<const void *>(dejavu_hook_set_result_object)},
        {"dejavu_hook_set_result_void", reinterpret_cast<const void *>(dejavu_hook_set_result_void)},
        {"dejavu_hook_get_arg_string_mutf8", reinterpret_cast<const void *>(dejavu_hook_get_arg_string_mutf8)},
        {"dejavu_hook_set_arg_string_mutf8", reinterpret_cast<const void *>(dejavu_hook_set_arg_string_mutf8)},
        {"dejavu_hook_get_result_string_mutf8", reinterpret_cast<const void *>(dejavu_hook_get_result_string_mutf8)},
        {"dejavu_hook_set_result_string_mutf8", reinterpret_cast<const void *>(dejavu_hook_set_result_string_mutf8)},
        {"dejavu_hook_new_string_mutf8", reinterpret_cast<const void *>(dejavu_hook_new_string_mutf8)},
        {"dejavu_hook_is_instance_of", reinterpret_cast<const void *>(dejavu_hook_is_instance_of)},
        {"dejavu_hook_call", reinterpret_cast<const void *>(dejavu_hook_call)},
        {"dejavu_hook_log", reinterpret_cast<const void *>(dejavu_hook_log)},
    };
    for (const HookSymbol &symbol : symbols) {
        if (runtime->engine_add_symbol(engine, symbol.name, symbol.address) != DEJAVU_OK) {
            set_error(error, "unable to register native hook helper symbols");
            return false;
        }
    }
    return true;
}

jobjectArray dejavu_hook_copy_arguments(JNIEnv *env, jobjectArray arguments) {
    const jsize count = arguments == nullptr ? 0 : env->GetArrayLength(arguments);
    jclass object_class = env->FindClass("java/lang/Object");
    jobjectArray copy = env->NewObjectArray(count, object_class, nullptr);
    if (copy == nullptr) {
        return nullptr;
    }
    for (jsize index = 0; index < count; ++index) {
        jobject value = env->GetObjectArrayElement(arguments, index);
        env->SetObjectArrayElement(copy, index, value);
        if (env->ExceptionCheck()) {
            return nullptr;
        }
    }
    return copy;
}




extern "C" void dejavu_hook_log(const char *message) {
    __android_log_print(ANDROID_LOG_DEBUG, kLogTag, "%s", message != nullptr ? message : "");
}
