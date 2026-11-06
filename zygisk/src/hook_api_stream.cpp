#include "hook_api_runtime_internal.h"

#include <jni.h>

#include <string>

#include "hook_event_sink.h"

using namespace dejavu_hook_internal;

namespace {

bool string_to_utf8(JNIEnv *env, jstring value, std::string *out) {
    jclass string_class = env->FindClass("java/lang/String");
    if (string_class == nullptr) {
        env->ExceptionClear();
        return false;
    }
    const bool is_string = env->IsInstanceOf(value, string_class) == JNI_TRUE;
    env->DeleteLocalRef(string_class);
    if (!is_string) {
        return false;
    }
    const char *chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) {
        env->ExceptionClear();
        return false;
    }
    const jsize length = env->GetStringUTFLength(value);
    if (env->ExceptionCheck() || length < 0) {
        env->ExceptionClear();
        env->ReleaseStringUTFChars(value, chars);
        return false;
    }
    out->assign(chars, static_cast<size_t>(length));
    env->ReleaseStringUTFChars(value, chars);
    return true;
}

}  // namespace

extern "C" int dejavu_hook_emit(
    dejavu_hook_context *context,
    const char *topic,
    void *string_object) {
    if (context == nullptr || context->env == nullptr || topic == nullptr) {
        return DEJAVU_HOOK_ERROR_ARGUMENT;
    }
    if (string_object == nullptr) {
        // Nothing to send; a missing payload is not a hook failure.
        return DEJAVU_HOOK_OK;
    }
    std::string text;
    if (!string_to_utf8(context->env, static_cast<jstring>(string_object), &text)) {
        // Drop silently: returning an error here would turn every event into a
        // logged hook failure, which the event channel must avoid.
        return DEJAVU_HOOK_OK;
    }
    HookEventSink::instance().emit(topic, text.data(), text.size());
    return DEJAVU_HOOK_OK;
}

extern "C" int dejavu_hook_emit_text(
    dejavu_hook_context *,
    const char *topic,
    const char *data,
    unsigned long size) {
    if (topic == nullptr || (data == nullptr && size != 0)) {
        return DEJAVU_HOOK_ERROR_ARGUMENT;
    }
    HookEventSink::instance().emit(topic, data, static_cast<size_t>(size));
    return DEJAVU_HOOK_OK;
}
