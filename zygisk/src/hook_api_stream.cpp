#include "hook_api_runtime_internal.h"

#include <jni.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "hook_event_sink.h"

using namespace dejavu_hook_internal;

namespace {

bool append_utf8(uint32_t value, std::string *out) {
    char bytes[4];
    size_t size = 0;
    if (value <= 0x7f) {
        bytes[size++] = static_cast<char>(value);
    } else if (value <= 0x7ff) {
        bytes[size++] = static_cast<char>(0xc0 | (value >> 6));
        bytes[size++] = static_cast<char>(0x80 | (value & 0x3f));
    } else if (value <= 0xffff) {
        bytes[size++] = static_cast<char>(0xe0 | (value >> 12));
        bytes[size++] = static_cast<char>(0x80 | ((value >> 6) & 0x3f));
        bytes[size++] = static_cast<char>(0x80 | (value & 0x3f));
    } else {
        bytes[size++] = static_cast<char>(0xf0 | (value >> 18));
        bytes[size++] = static_cast<char>(0x80 | ((value >> 12) & 0x3f));
        bytes[size++] = static_cast<char>(0x80 | ((value >> 6) & 0x3f));
        bytes[size++] = static_cast<char>(0x80 | (value & 0x3f));
    }
    if (out->size() > HookEventSink::kMaximumPayloadBytes - size)
        return false;
    out->append(bytes, size);
    return true;
}

bool string_to_utf8(JNIEnv *env, jstring value, std::string *out) {
    jclass string_class = env->FindClass("java/lang/String");
    if (string_class == nullptr) {
        env->ExceptionClear();
        return false;
    }
    const bool is_string = env->IsInstanceOf(value, string_class) == JNI_TRUE;
    env->DeleteLocalRef(string_class);
    if (!is_string)
        return false;
    const jsize length = env->GetStringLength(value);
    if (env->ExceptionCheck() || length < 0) {
        env->ExceptionClear();
        return false;
    }
    // Each UTF-16 code unit requires at least one output byte. Reject large
    // inputs before JNI can allocate or pin a whole-string character copy.
    if (static_cast<size_t>(length) > HookEventSink::kMaximumPayloadBytes)
        return false;
    out->clear();
    out->reserve(static_cast<size_t>(length));
    jchar units[1024];
    uint32_t high_surrogate = 0;
    for (jsize start = 0; start < length;) {
        const jsize count = std::min<jsize>(length - start, 1024);
        env->GetStringRegion(value, start, count, units);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            return false;
        }
        for (jsize index = 0; index < count; ++index) {
            const uint32_t unit = units[index];
            if (high_surrogate != 0) {
                const uint32_t high = high_surrogate;
                high_surrogate = 0;
                if (unit >= 0xdc00 && unit <= 0xdfff) {
                    if (!append_utf8(0x10000 + ((high - 0xd800) << 10) + unit - 0xdc00, out))
                        return false;
                    continue;
                }
                if (!append_utf8(0xfffd, out))
                    return false;
            }
            if (unit >= 0xd800 && unit <= 0xdbff) {
                high_surrogate = unit;
            } else if (!append_utf8(unit >= 0xdc00 && unit <= 0xdfff ? 0xfffd : unit, out)) {
                return false;
            }
        }
        start += count;
    }
    // An unpaired Java surrogate has no Unicode scalar value. Emit the standard
    // replacement character rather than placing invalid UTF-8 on the wire.
    return high_surrogate == 0 || append_utf8(0xfffd, out);
}

} // namespace

extern "C" int dejavu_hook_emit(dejavu_hook_context *context, const char *topic,
                                void *string_object) {
    if (context == nullptr || context->env == nullptr || topic == nullptr) {
        return DEJAVU_HOOK_ERROR_ARGUMENT;
    }
    if (string_object == nullptr) {
        // Nothing to send; a missing payload is not a hook failure.
        return DEJAVU_HOOK_OK;
    }
    auto &sink = HookEventSink::instance();
    if (!sink.active()) {
        sink.record_drop();
        return DEJAVU_HOOK_OK;
    }
    std::string text;
    if (!string_to_utf8(context->env, static_cast<jstring>(string_object), &text)) {
        // Drop silently: returning an error here would turn every event into a
        // logged hook failure, which the event channel must avoid.
        sink.record_drop();
        return DEJAVU_HOOK_OK;
    }
    sink.emit(topic, text.data(), text.size());
    return DEJAVU_HOOK_OK;
}

extern "C" int dejavu_hook_emit_text(dejavu_hook_context *, const char *topic, const char *data,
                                     unsigned long size) {
    if (topic == nullptr || (data == nullptr && size != 0)) {
        return DEJAVU_HOOK_ERROR_ARGUMENT;
    }
    HookEventSink::instance().emit(topic, data, static_cast<size_t>(size));
    return DEJAVU_HOOK_OK;
}
