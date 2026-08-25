#include "hook_api_runtime.h"

#include <android/log.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

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

int primitive_index(char kind) {
    switch (kind) {
        case 'Z': return 0;
        case 'B': return 1;
        case 'C': return 2;
        case 'S': return 3;
        case 'I': return 4;
        case 'J': return 5;
        case 'F': return 6;
        case 'D': return 7;
        default: return -1;
    }
}

bool is_integral_kind(char kind) {
    return kind == 'Z' || kind == 'B' || kind == 'C' || kind == 'S' ||
        kind == 'I' || kind == 'J';
}

bool is_floating_kind(char kind) {
    return kind == 'F' || kind == 'D';
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

bool integral_in_range(char kind, long long value) {
    switch (kind) {
        case 'Z': return value == 0 || value == 1;
        case 'B':
            return value >= std::numeric_limits<jbyte>::min() &&
                value <= std::numeric_limits<jbyte>::max();
        case 'C':
            return value >= std::numeric_limits<jchar>::min() &&
                value <= std::numeric_limits<jchar>::max();
        case 'S':
            return value >= std::numeric_limits<jshort>::min() &&
                value <= std::numeric_limits<jshort>::max();
        case 'I':
            return value >= std::numeric_limits<jint>::min() &&
                value <= std::numeric_limits<jint>::max();
        case 'J': return true;
        default: return false;
    }
}

jobject box_integral(dejavu_hook_context *context, char kind, long long value) {
    const int index = primitive_index(kind);
    if (index < 0 || !integral_in_range(kind, value)) {
        mark_context_error(
            context,
            is_integral_kind(kind) ? DEJAVU_HOOK_ERROR_RANGE : DEJAVU_HOOK_ERROR_TYPE);
        return nullptr;
    }
    jvalue argument = {};
    switch (kind) {
        case 'Z': argument.z = static_cast<jboolean>(value); break;
        case 'B': argument.b = static_cast<jbyte>(value); break;
        case 'C': argument.c = static_cast<jchar>(value); break;
        case 'S': argument.s = static_cast<jshort>(value); break;
        case 'I': argument.i = static_cast<jint>(value); break;
        case 'J': argument.j = static_cast<jlong>(value); break;
        default: return nullptr;
    }
    const DejavuPrimitiveApi &api = context->jni->primitives[static_cast<size_t>(index)];
    jobject boxed = context->env->CallStaticObjectMethodA(api.type, api.box, &argument);
    if (context->env->ExceptionCheck() || boxed == nullptr) {
        context->env->ExceptionClear();
        mark_context_error(context, DEJAVU_HOOK_ERROR_JNI);
        return nullptr;
    }
    return boxed;
}

jobject box_floating(dejavu_hook_context *context, char kind, double value) {
    const int index = primitive_index(kind);
    if (index < 0 || !is_floating_kind(kind)) {
        mark_context_error(context, DEJAVU_HOOK_ERROR_TYPE);
        return nullptr;
    }
    jvalue argument = {};
    if (kind == 'F') {
        argument.f = static_cast<jfloat>(value);
    } else {
        argument.d = static_cast<jdouble>(value);
    }
    const DejavuPrimitiveApi &api = context->jni->primitives[static_cast<size_t>(index)];
    jobject boxed = context->env->CallStaticObjectMethodA(api.type, api.box, &argument);
    if (context->env->ExceptionCheck() || boxed == nullptr) {
        context->env->ExceptionClear();
        mark_context_error(context, DEJAVU_HOOK_ERROR_JNI);
        return nullptr;
    }
    return boxed;
}

int unbox_integral(
    dejavu_hook_context *context, char kind, jobject boxed, long long *value) {
    if (value == nullptr || boxed == nullptr || !is_integral_kind(kind)) {
        return mark_context_error(
            context,
            is_integral_kind(kind) ? DEJAVU_HOOK_ERROR_ARGUMENT : DEJAVU_HOOK_ERROR_TYPE);
    }
    const DejavuPrimitiveApi &api =
        context->jni->primitives[static_cast<size_t>(primitive_index(kind))];
    switch (kind) {
        case 'Z': *value = context->env->CallBooleanMethodA(boxed, api.unbox, nullptr); break;
        case 'B': *value = context->env->CallByteMethodA(boxed, api.unbox, nullptr); break;
        case 'C': *value = context->env->CallCharMethodA(boxed, api.unbox, nullptr); break;
        case 'S': *value = context->env->CallShortMethodA(boxed, api.unbox, nullptr); break;
        case 'I': *value = context->env->CallIntMethodA(boxed, api.unbox, nullptr); break;
        case 'J': *value = context->env->CallLongMethodA(boxed, api.unbox, nullptr); break;
        default: return mark_context_error(context, DEJAVU_HOOK_ERROR_TYPE);
    }
    if (context->env->ExceptionCheck()) {
        context->env->ExceptionClear();
        return mark_context_error(context, DEJAVU_HOOK_ERROR_JNI);
    }
    return DEJAVU_HOOK_OK;
}

int unbox_floating(dejavu_hook_context *context, char kind, jobject boxed, double *value) {
    if (value == nullptr || boxed == nullptr || !is_floating_kind(kind)) {
        return mark_context_error(
            context,
            is_floating_kind(kind) ? DEJAVU_HOOK_ERROR_ARGUMENT : DEJAVU_HOOK_ERROR_TYPE);
    }
    const DejavuPrimitiveApi &api =
        context->jni->primitives[static_cast<size_t>(primitive_index(kind))];
    *value = kind == 'F'
        ? static_cast<double>(context->env->CallFloatMethodA(boxed, api.unbox, nullptr))
        : context->env->CallDoubleMethodA(boxed, api.unbox, nullptr);
    if (context->env->ExceptionCheck()) {
        context->env->ExceptionClear();
        return mark_context_error(context, DEJAVU_HOOK_ERROR_JNI);
    }
    return DEJAVU_HOOK_OK;
}

int replace_result(dejavu_hook_context *context, jobject result) {
    if (context->result_owned && context->result != nullptr) {
        context->env->DeleteLocalRef(context->result);
    }
    context->result = result;
    context->result_owned = result != nullptr;
    context->result_set = true;
    return DEJAVU_HOOK_OK;
}

bool is_object_kind(char kind) {
    return kind == 'L';
}

void clear_local(JNIEnv *env, jobject value) {
    if (env != nullptr && value != nullptr) {
        env->DeleteLocalRef(value);
    }
}

jobject resolve_class(
    dejavu_hook_context *context,
    const char *class_name,
    bool *failed) {
    *failed = false;
    if (context == nullptr || context->env == nullptr || class_name == nullptr ||
        class_name[0] == '\0') {
        *failed = true;
        return nullptr;
    }
    JNIEnv *env = context->env;
    std::string name(class_name);
    for (char &character : name) {
        if (character == '/') {
            character = '.';
        }
    }
    jobject result = nullptr;
    if (context->class_loader == nullptr) {
        std::replace(name.begin(), name.end(), '.', '/');
        result = env->FindClass(name.c_str());
    } else {
        jclass loader_class = env->FindClass("java/lang/ClassLoader");
        jmethodID load = loader_class == nullptr
            ? nullptr
            : env->GetMethodID(
                  loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
        jstring java_name = load == nullptr ? nullptr : env->NewStringUTF(name.c_str());
        result = java_name == nullptr || load == nullptr
            ? nullptr
            : env->CallObjectMethod(context->class_loader, load, java_name);
        clear_local(env, java_name);
        clear_local(env, loader_class);
    }
    if (env->ExceptionCheck() || result == nullptr) {
        env->ExceptionClear();
        *failed = true;
        clear_local(env, result);
        return nullptr;
    }
    return result;
}

int copy_string_mutf8(
    dejavu_hook_context *context,
    jstring value,
    char *buffer,
    size_t capacity,
    size_t *size) {
    if (size == nullptr || (capacity != 0 && buffer == nullptr)) {
        return mark_context_error(context, DEJAVU_HOOK_ERROR_ARGUMENT);
    }
    if (value == nullptr) {
        return mark_context_error(context, DEJAVU_HOOK_ERROR_TYPE);
    }
    JNIEnv *env = context->env;
    jclass string_class = env->FindClass("java/lang/String");
    if (string_class == nullptr || !env->IsInstanceOf(value, string_class)) {
        env->ExceptionClear();
        clear_local(env, string_class);
        return mark_context_error(context, DEJAVU_HOOK_ERROR_TYPE);
    }
    clear_local(env, string_class);
    const jsize length = env->GetStringUTFLength(value);
    if (env->ExceptionCheck() || length < 0) {
        env->ExceptionClear();
        return mark_context_error(context, DEJAVU_HOOK_ERROR_JNI);
    }
    *size = static_cast<size_t>(length);
    if (capacity == 0) {
        return DEJAVU_HOOK_OK;
    }
    if (capacity <= *size) {
        if (buffer != nullptr) {
            buffer[0] = '\0';
        }
        return mark_context_error(context, DEJAVU_HOOK_ERROR_RANGE);
    }
    const jsize utf16_length = env->GetStringLength(value);
    if (env->ExceptionCheck() || utf16_length < 0) {
        env->ExceptionClear();
        return mark_context_error(context, DEJAVU_HOOK_ERROR_JNI);
    }
    env->GetStringUTFRegion(value, 0, utf16_length, buffer);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return mark_context_error(context, DEJAVU_HOOK_ERROR_JNI);
    }
    buffer[*size] = '\0';
    return DEJAVU_HOOK_OK;
}

jstring new_string_mutf8(
    dejavu_hook_context *context,
    const char *data,
    size_t size) {
    if (data == nullptr && size != 0) {
        mark_context_error(context, DEJAVU_HOOK_ERROR_ARGUMENT);
        return nullptr;
    }
    std::string copy(data == nullptr ? "" : data, size);
    jstring result = context->env->NewStringUTF(copy.c_str());
    if (context->env->ExceptionCheck() || result == nullptr) {
        context->env->ExceptionClear();
        mark_context_error(context, DEJAVU_HOOK_ERROR_JNI);
        return nullptr;
    }
    return result;
}

int value_to_jvalue(
    dejavu_hook_context *context,
    char kind,
    const dejavu_hook_value &value,
    jvalue *out,
    jobject *temporary) {
    *temporary = nullptr;
    if (kind == 'V' || out == nullptr) {
        return mark_context_error(context, DEJAVU_HOOK_ERROR_ARGUMENT);
    }
    if (is_integral_kind(kind)) {
        if (value.kind != DEJAVU_HOOK_VALUE_I64 &&
            !(kind == 'Z' && value.kind == DEJAVU_HOOK_VALUE_BOOLEAN)) {
            return mark_context_error(context, DEJAVU_HOOK_ERROR_TYPE);
        }
        const long long raw = value.kind == DEJAVU_HOOK_VALUE_I64
            ? value.value.i64_value
            : value.value.boolean_value;
        if (!integral_in_range(kind, raw)) {
            return mark_context_error(context, DEJAVU_HOOK_ERROR_RANGE);
        }
        switch (kind) {
            case 'Z': out->z = static_cast<jboolean>(raw); break;
            case 'B': out->b = static_cast<jbyte>(raw); break;
            case 'C': out->c = static_cast<jchar>(raw); break;
            case 'S': out->s = static_cast<jshort>(raw); break;
            case 'I': out->i = static_cast<jint>(raw); break;
            case 'J': out->j = static_cast<jlong>(raw); break;
            default: return mark_context_error(context, DEJAVU_HOOK_ERROR_TYPE);
        }
        return DEJAVU_HOOK_OK;
    }
    if (is_floating_kind(kind)) {
        if (value.kind != DEJAVU_HOOK_VALUE_F64) {
            return mark_context_error(context, DEJAVU_HOOK_ERROR_TYPE);
        }
        if (kind == 'F') {
            out->f = static_cast<jfloat>(value.value.f64_value);
        } else {
            out->d = static_cast<jdouble>(value.value.f64_value);
        }
        return DEJAVU_HOOK_OK;
    }
    if (is_object_kind(kind)) {
        if (value.kind == DEJAVU_HOOK_VALUE_OBJECT) {
            out->l = static_cast<jobject>(value.value.object_value);
            return DEJAVU_HOOK_OK;
        }
        if (value.kind == DEJAVU_HOOK_VALUE_STRING) {
            *temporary = new_string_mutf8(
                context, value.value.string_value.data, value.value.string_value.size);
            if (*temporary == nullptr) {
                return context->helper_status;
            }
            out->l = *temporary;
            return DEJAVU_HOOK_OK;
        }
        return mark_context_error(context, DEJAVU_HOOK_ERROR_TYPE);
    }
    return mark_context_error(context, DEJAVU_HOOK_ERROR_TYPE);
}

}  // namespace

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

extern "C" unsigned int dejavu_hook_abi_version(void) {
    return DEJAVU_HOOK_ABI_VERSION;
}

extern "C" unsigned int dejavu_hook_phase(const dejavu_hook_context *context) {
    return context == nullptr ? 0 : context->phase;
}

extern "C" unsigned long long dejavu_hook_id(const dejavu_hook_context *context) {
    return context == nullptr ? 0 : context->id;
}

extern "C" unsigned int dejavu_hook_arg_count(const dejavu_hook_context *context) {
    return context == nullptr || context->signature == nullptr
        ? 0
        : static_cast<unsigned int>(context->signature->parameters.size());
}

extern "C" void *dejavu_hook_this_object(const dejavu_hook_context *context) {
    return context == nullptr ? nullptr : context->this_object;
}

extern "C" int dejavu_hook_get_arg_i64(
    dejavu_hook_context *context, unsigned int index, long long *value) {
    char kind = '\0';
    jobject boxed = nullptr;
    const int status = check_argument(context, index, &kind, &boxed);
    return status == DEJAVU_HOOK_OK
        ? unbox_integral(context, kind, boxed, value)
        : status;
}

extern "C" int dejavu_hook_set_arg_i64(
    dejavu_hook_context *context, unsigned int index, long long value) {
    if (check_context(context) != DEJAVU_HOOK_OK ||
        context->phase != DEJAVU_HOOK_PHASE_BEFORE ||
        index >= context->signature->parameters.size()) {
        return mark_context_error(
            context,
            context != nullptr && context->phase != DEJAVU_HOOK_PHASE_BEFORE
                ? DEJAVU_HOOK_ERROR_PHASE
                : DEJAVU_HOOK_ERROR_ARGUMENT);
    }
    const char kind = context->signature->parameters[index];
    jobject boxed = box_integral(context, kind, value);
    if (boxed == nullptr) {
        return context->helper_status;
    }
    context->env->SetObjectArrayElement(
        context->arguments,
        static_cast<jsize>(context->argument_offset + index),
        boxed);
    if (context->env->ExceptionCheck()) {
        context->env->ExceptionClear();
        return mark_context_error(context, DEJAVU_HOOK_ERROR_JNI);
    }
    return DEJAVU_HOOK_OK;
}

extern "C" int dejavu_hook_get_arg_f64(
    dejavu_hook_context *context, unsigned int index, double *value) {
    char kind = '\0';
    jobject boxed = nullptr;
    const int status = check_argument(context, index, &kind, &boxed);
    return status == DEJAVU_HOOK_OK
        ? unbox_floating(context, kind, boxed, value)
        : status;
}

extern "C" int dejavu_hook_set_arg_f64(
    dejavu_hook_context *context, unsigned int index, double value) {
    if (check_context(context) != DEJAVU_HOOK_OK ||
        context->phase != DEJAVU_HOOK_PHASE_BEFORE ||
        index >= context->signature->parameters.size()) {
        return mark_context_error(
            context,
            context != nullptr && context->phase != DEJAVU_HOOK_PHASE_BEFORE
                ? DEJAVU_HOOK_ERROR_PHASE
                : DEJAVU_HOOK_ERROR_ARGUMENT);
    }
    const char kind = context->signature->parameters[index];
    jobject boxed = box_floating(context, kind, value);
    if (boxed == nullptr) {
        return context->helper_status;
    }
    context->env->SetObjectArrayElement(
        context->arguments,
        static_cast<jsize>(context->argument_offset + index),
        boxed);
    if (context->env->ExceptionCheck()) {
        context->env->ExceptionClear();
        return mark_context_error(context, DEJAVU_HOOK_ERROR_JNI);
    }
    return DEJAVU_HOOK_OK;
}

extern "C" int dejavu_hook_get_arg_object(
    dejavu_hook_context *context, unsigned int index, void **value) {
    if (value == nullptr) {
        return mark_context_error(context, DEJAVU_HOOK_ERROR_ARGUMENT);
    }
    char kind = '\0';
    jobject boxed = nullptr;
    const int status = check_argument(context, index, &kind, &boxed);
    if (status != DEJAVU_HOOK_OK) {
        return status;
    }
    if (kind != 'L') {
        return mark_context_error(context, DEJAVU_HOOK_ERROR_TYPE);
    }
    *value = boxed;
    return DEJAVU_HOOK_OK;
}

extern "C" int dejavu_hook_set_arg_object(
    dejavu_hook_context *context, unsigned int index, void *value) {
    if (check_context(context) != DEJAVU_HOOK_OK ||
        context->phase != DEJAVU_HOOK_PHASE_BEFORE ||
        index >= context->signature->parameters.size()) {
        return mark_context_error(
            context,
            context != nullptr && context->phase != DEJAVU_HOOK_PHASE_BEFORE
                ? DEJAVU_HOOK_ERROR_PHASE
                : DEJAVU_HOOK_ERROR_ARGUMENT);
    }
    if (context->signature->parameters[index] != 'L') {
        return mark_context_error(context, DEJAVU_HOOK_ERROR_TYPE);
    }
    context->env->SetObjectArrayElement(
        context->arguments,
        static_cast<jsize>(context->argument_offset + index),
        static_cast<jobject>(value));
    if (context->env->ExceptionCheck()) {
        context->env->ExceptionClear();
        return mark_context_error(context, DEJAVU_HOOK_ERROR_JNI);
    }
    return DEJAVU_HOOK_OK;
}

extern "C" int dejavu_hook_get_result_i64(
    dejavu_hook_context *context, long long *value) {
    if (check_context(context) != DEJAVU_HOOK_OK ||
        context->phase != DEJAVU_HOOK_PHASE_AFTER) {
        return mark_context_error(context, DEJAVU_HOOK_ERROR_PHASE);
    }
    return unbox_integral(context, context->signature->result, context->result, value);
}

extern "C" int dejavu_hook_set_result_i64(
    dejavu_hook_context *context, long long value) {
    if (check_context(context) != DEJAVU_HOOK_OK) {
        return DEJAVU_HOOK_ERROR_ARGUMENT;
    }
    jobject result = box_integral(context, context->signature->result, value);
    return result == nullptr ? context->helper_status : replace_result(context, result);
}

extern "C" int dejavu_hook_get_result_f64(
    dejavu_hook_context *context, double *value) {
    if (check_context(context) != DEJAVU_HOOK_OK ||
        context->phase != DEJAVU_HOOK_PHASE_AFTER) {
        return mark_context_error(context, DEJAVU_HOOK_ERROR_PHASE);
    }
    return unbox_floating(context, context->signature->result, context->result, value);
}

extern "C" int dejavu_hook_set_result_f64(
    dejavu_hook_context *context, double value) {
    if (check_context(context) != DEJAVU_HOOK_OK) {
        return DEJAVU_HOOK_ERROR_ARGUMENT;
    }
    jobject result = box_floating(context, context->signature->result, value);
    return result == nullptr ? context->helper_status : replace_result(context, result);
}

extern "C" int dejavu_hook_get_result_object(
    dejavu_hook_context *context, void **value) {
    if (check_context(context) != DEJAVU_HOOK_OK || value == nullptr) {
        return mark_context_error(context, DEJAVU_HOOK_ERROR_ARGUMENT);
    }
    if (context->phase != DEJAVU_HOOK_PHASE_AFTER) {
        return mark_context_error(context, DEJAVU_HOOK_ERROR_PHASE);
    }
    if (context->signature->result != 'L') {
        return mark_context_error(context, DEJAVU_HOOK_ERROR_TYPE);
    }
    *value = context->result;
    return DEJAVU_HOOK_OK;
}

extern "C" int dejavu_hook_set_result_object(
    dejavu_hook_context *context, void *value) {
    if (check_context(context) != DEJAVU_HOOK_OK) {
        return DEJAVU_HOOK_ERROR_ARGUMENT;
    }
    if (context->signature->result != 'L') {
        return mark_context_error(context, DEJAVU_HOOK_ERROR_TYPE);
    }
    jobject result = value == nullptr
        ? nullptr
        : context->env->NewLocalRef(static_cast<jobject>(value));
    if (context->env->ExceptionCheck()) {
        context->env->ExceptionClear();
        return mark_context_error(context, DEJAVU_HOOK_ERROR_JNI);
    }
    return replace_result(context, result);
}

extern "C" int dejavu_hook_set_result_void(dejavu_hook_context *context) {
    if (check_context(context) != DEJAVU_HOOK_OK) {
        return DEJAVU_HOOK_ERROR_ARGUMENT;
    }
    if (context->signature->result != 'V') {
        return mark_context_error(context, DEJAVU_HOOK_ERROR_TYPE);
    }
    return replace_result(context, nullptr);
}

extern "C" int dejavu_hook_get_arg_string_mutf8(
    dejavu_hook_context *context,
    unsigned int index,
    char *buffer,
    size_t capacity,
    size_t *size) {
    char kind = '\0';
    jobject value = nullptr;
    const int status = check_argument(context, index, &kind, &value);
    if (status != DEJAVU_HOOK_OK) {
        return status;
    }
    if (kind != 'L') {
        return mark_context_error(context, DEJAVU_HOOK_ERROR_TYPE);
    }
    return copy_string_mutf8(context, static_cast<jstring>(value), buffer, capacity, size);
}

extern "C" int dejavu_hook_set_arg_string_mutf8(
    dejavu_hook_context *context,
    unsigned int index,
    const char *data,
    size_t size) {
    if (check_context(context) != DEJAVU_HOOK_OK ||
        context->phase != DEJAVU_HOOK_PHASE_BEFORE ||
        index >= context->signature->parameters.size()) {
        return mark_context_error(
            context,
            context != nullptr && context->phase != DEJAVU_HOOK_PHASE_BEFORE
                ? DEJAVU_HOOK_ERROR_PHASE
                : DEJAVU_HOOK_ERROR_ARGUMENT);
    }
    if (context->signature->parameters[index] != 'L') {
        return mark_context_error(context, DEJAVU_HOOK_ERROR_TYPE);
    }
    jstring value = new_string_mutf8(context, data, size);
    if (value == nullptr) {
        return context->helper_status;
    }
    context->env->SetObjectArrayElement(
        context->arguments,
        static_cast<jsize>(context->argument_offset + index),
        value);
    clear_local(context->env, value);
    if (context->env->ExceptionCheck()) {
        context->env->ExceptionClear();
        return mark_context_error(context, DEJAVU_HOOK_ERROR_JNI);
    }
    return DEJAVU_HOOK_OK;
}

extern "C" int dejavu_hook_get_result_string_mutf8(
    dejavu_hook_context *context,
    char *buffer,
    size_t capacity,
    size_t *size) {
    if (check_context(context) != DEJAVU_HOOK_OK ||
        context->phase != DEJAVU_HOOK_PHASE_AFTER) {
        return mark_context_error(context, DEJAVU_HOOK_ERROR_PHASE);
    }
    if (context->signature->result != 'L') {
        return mark_context_error(context, DEJAVU_HOOK_ERROR_TYPE);
    }
    return copy_string_mutf8(
        context, static_cast<jstring>(context->result), buffer, capacity, size);
}

extern "C" int dejavu_hook_set_result_string_mutf8(
    dejavu_hook_context *context,
    const char *data,
    size_t size) {
    if (check_context(context) != DEJAVU_HOOK_OK) {
        return DEJAVU_HOOK_ERROR_ARGUMENT;
    }
    if (context->signature->result != 'L') {
        return mark_context_error(context, DEJAVU_HOOK_ERROR_TYPE);
    }
    jstring value = new_string_mutf8(context, data, size);
    return value == nullptr ? context->helper_status : replace_result(context, value);
}

extern "C" int dejavu_hook_new_string_mutf8(
    dejavu_hook_context *context,
    const char *data,
    size_t size,
    void **value) {
    if (check_context(context) != DEJAVU_HOOK_OK || value == nullptr) {
        return mark_context_error(context, DEJAVU_HOOK_ERROR_ARGUMENT);
    }
    jstring result = new_string_mutf8(context, data, size);
    if (result == nullptr) {
        return context->helper_status;
    }
    *value = result;
    return DEJAVU_HOOK_OK;
}

extern "C" int dejavu_hook_is_instance_of(
    dejavu_hook_context *context,
    void *object,
    const char *class_name,
    int *result) {
    if (check_context(context) != DEJAVU_HOOK_OK || result == nullptr ||
        class_name == nullptr) {
        return mark_context_error(context, DEJAVU_HOOK_ERROR_ARGUMENT);
    }
    bool failed = false;
    jobject klass = resolve_class(context, class_name, &failed);
    if (failed || klass == nullptr) {
        return mark_context_error(context, DEJAVU_HOOK_ERROR_JNI);
    }
    *result = object != nullptr &&
        context->env->IsInstanceOf(static_cast<jobject>(object), static_cast<jclass>(klass));
    clear_local(context->env, klass);
    if (context->env->ExceptionCheck()) {
        context->env->ExceptionClear();
        return mark_context_error(context, DEJAVU_HOOK_ERROR_JNI);
    }
    return DEJAVU_HOOK_OK;
}

extern "C" int dejavu_hook_call(
    dejavu_hook_context *context,
    unsigned int flags,
    void *receiver,
    const char *class_name,
    const char *method_name,
    const char *signature,
    const dejavu_hook_value *arguments,
    unsigned int argument_count,
    dejavu_hook_value *result) {
    if (check_context(context) != DEJAVU_HOOK_OK || class_name == nullptr ||
        method_name == nullptr || signature == nullptr || result == nullptr ||
        (argument_count != 0 && arguments == nullptr)) {
        return mark_context_error(context, DEJAVU_HOOK_ERROR_ARGUMENT);
    }
    const bool is_static = (flags & DEJAVU_HOOK_CALL_STATIC) != 0;
    DejavuHookSignature parsed;
    std::string parse_error;
    if (!dejavu_parse_hook_signature(signature, &parsed, &parse_error) ||
        parsed.parameters.size() != argument_count ||
        (!is_static && receiver == nullptr)) {
        return mark_context_error(context, DEJAVU_HOOK_ERROR_ARGUMENT);
    }
    bool failed = false;
    jobject klass = resolve_class(context, class_name, &failed);
    if (failed || klass == nullptr) {
        return mark_context_error(context, DEJAVU_HOOK_ERROR_JNI);
    }
    jmethodID method = is_static
        ? context->env->GetStaticMethodID(
              static_cast<jclass>(klass), method_name, signature)
        : context->env->GetMethodID(static_cast<jclass>(klass), method_name, signature);
    if (method == nullptr || context->env->ExceptionCheck()) {
        context->env->ExceptionClear();
        clear_local(context->env, klass);
        return mark_context_error(context, DEJAVU_HOOK_ERROR_JNI);
    }
    std::vector<jvalue> values(argument_count);
    std::vector<jobject> temporary;
    temporary.reserve(argument_count);
    for (unsigned int index = 0; index < argument_count; ++index) {
        jobject local = nullptr;
        const int status = value_to_jvalue(
            context, parsed.parameters[index], arguments[index], &values[index], &local);
        if (status != DEJAVU_HOOK_OK) {
            for (jobject item : temporary) clear_local(context->env, item);
            clear_local(context->env, klass);
            return status;
        }
        if (local != nullptr) temporary.push_back(local);
    }

    jobject object_result = nullptr;
    switch (parsed.result) {
        case 'V':
            if (is_static) {
                context->env->CallStaticVoidMethodA(
                    static_cast<jclass>(klass), method, values.data());
            } else {
                context->env->CallVoidMethodA(
                    static_cast<jobject>(receiver), method, values.data());
            }
            result->kind = DEJAVU_HOOK_VALUE_VOID;
            break;
        case 'Z':
            result->kind = DEJAVU_HOOK_VALUE_BOOLEAN;
            result->value.boolean_value = is_static
                ? context->env->CallStaticBooleanMethodA(
                      static_cast<jclass>(klass), method, values.data())
                : context->env->CallBooleanMethodA(
                      static_cast<jobject>(receiver), method, values.data());
            break;
        case 'B': case 'C': case 'S': case 'I': case 'J':
            result->kind = DEJAVU_HOOK_VALUE_I64;
            switch (parsed.result) {
                case 'B': result->value.i64_value = is_static
                    ? context->env->CallStaticByteMethodA(static_cast<jclass>(klass), method, values.data())
                    : context->env->CallByteMethodA(static_cast<jobject>(receiver), method, values.data()); break;
                case 'C': result->value.i64_value = is_static
                    ? context->env->CallStaticCharMethodA(static_cast<jclass>(klass), method, values.data())
                    : context->env->CallCharMethodA(static_cast<jobject>(receiver), method, values.data()); break;
                case 'S': result->value.i64_value = is_static
                    ? context->env->CallStaticShortMethodA(static_cast<jclass>(klass), method, values.data())
                    : context->env->CallShortMethodA(static_cast<jobject>(receiver), method, values.data()); break;
                case 'I': result->value.i64_value = is_static
                    ? context->env->CallStaticIntMethodA(static_cast<jclass>(klass), method, values.data())
                    : context->env->CallIntMethodA(static_cast<jobject>(receiver), method, values.data()); break;
                default: result->value.i64_value = is_static
                    ? context->env->CallStaticLongMethodA(static_cast<jclass>(klass), method, values.data())
                    : context->env->CallLongMethodA(static_cast<jobject>(receiver), method, values.data()); break;
            }
            break;
        case 'F':
            result->kind = DEJAVU_HOOK_VALUE_F64;
            result->value.f64_value = is_static
                ? context->env->CallStaticFloatMethodA(static_cast<jclass>(klass), method, values.data())
                : context->env->CallFloatMethodA(static_cast<jobject>(receiver), method, values.data());
            break;
        case 'D':
            result->kind = DEJAVU_HOOK_VALUE_F64;
            result->value.f64_value = is_static
                ? context->env->CallStaticDoubleMethodA(static_cast<jclass>(klass), method, values.data())
                : context->env->CallDoubleMethodA(static_cast<jobject>(receiver), method, values.data());
            break;
        case 'L':
            object_result = is_static
                ? context->env->CallStaticObjectMethodA(
                      static_cast<jclass>(klass), method, values.data())
                : context->env->CallObjectMethodA(
                      static_cast<jobject>(receiver), method, values.data());
            result->kind = DEJAVU_HOOK_VALUE_OBJECT;
            result->value.object_value = object_result;
            break;
        default:
            for (jobject item : temporary) clear_local(context->env, item);
            clear_local(context->env, klass);
            return mark_context_error(context, DEJAVU_HOOK_ERROR_TYPE);
    }
    for (jobject item : temporary) clear_local(context->env, item);
    clear_local(context->env, klass);
    if (context->env->ExceptionCheck()) {
        context->env->ExceptionClear();
        return mark_context_error(context, DEJAVU_HOOK_ERROR_JNI);
    }
    return DEJAVU_HOOK_OK;
}

extern "C" void dejavu_hook_log(const char *message) {
    __android_log_print(ANDROID_LOG_DEBUG, kLogTag, "%s", message != nullptr ? message : "");
}
