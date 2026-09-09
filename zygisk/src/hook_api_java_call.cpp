#include "hook_api_runtime_internal.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace dejavu_hook_internal;

namespace dejavu_hook_internal {

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

}  // namespace dejavu_hook_internal
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
