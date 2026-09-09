#include "hook_api_runtime_internal.h"

#include <string>

using namespace dejavu_hook_internal;

namespace dejavu_hook_internal {

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

}  // namespace dejavu_hook_internal
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
