#pragma once
#include <functional>
#include <string_view>
#include <jni.h>
namespace lsplant {
struct InitInfo {
    std::function<void *(void *, void *)> inline_hooker;
    std::function<bool(void *)> inline_unhooker;
    std::function<void *(std::string_view)> art_symbol_resolver;
    std::function<void *(std::string_view)> art_symbol_prefix_resolver;
    const char *generated_class_name;
    const char *generated_source_name;
    const char *generated_field_name;
    const char *generated_method_name;
    std::function<void *()> executable_memory_allocator;
    std::function<void()> executable_memory_recycler;
};
inline bool Init(JNIEnv *, const InitInfo &) { return false; }
inline jobject Hook(JNIEnv *, jobject, jobject, jobject) { return nullptr; }
inline bool IsHooked(JNIEnv *, jobject) { return false; }
inline bool UnHook(JNIEnv *, jobject) { return false; }
inline bool Deoptimize(JNIEnv *, jobject) { return false; }
}
