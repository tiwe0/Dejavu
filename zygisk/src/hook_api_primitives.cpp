#include "hook_api_runtime_internal.h"

#include <limits>

using namespace dejavu_hook_internal;

namespace dejavu_hook_internal {

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

}  // namespace dejavu_hook_internal
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
