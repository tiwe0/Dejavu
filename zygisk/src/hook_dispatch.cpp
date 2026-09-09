#include "hook_manager_internal.h"

#include <android/log.h>

#include <atomic>
#include <sched.h>

#include "hook_api_runtime.h"

namespace {
constexpr char kLogTag[] = "DejavuHook";
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, kLogTag, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)
}

using namespace dejavu_hook_manager_internal;

jobject HookManager::dispatch(
    JNIEnv *env,
    unsigned long long hook_id,
    jobjectArray arguments) {
    if (impl_ == nullptr) {
        return nullptr;
    }
    Impl::Record *record = reinterpret_cast<Impl::Record *>(
        static_cast<uintptr_t>(hook_id));
    if (record == nullptr || record->manager != impl_) {
        return nullptr;
    }
    Impl::DispatchGuard dispatch_guard(record);
    while (!record->ready.load(std::memory_order_acquire)) {
        sched_yield();
    }
    if (record->bootstrap) {
        jobject result = call_original(env, record->backup, arguments, record->is_static);
        if (env->ExceptionCheck() || record->bootstrap_callback == nullptr ||
            record->bootstrap_started.exchange(true, std::memory_order_acq_rel)) {
            return result;
        }

        LOGD("app bootstrap dispatch entered");
        jclass activity_thread = env->FindClass("android/app/ActivityThread");
        jmethodID current_application = activity_thread == nullptr
            ? nullptr
            : env->GetStaticMethodID(
                  activity_thread,
                  "currentApplication",
                  "()Landroid/app/Application;");
        jobject application = current_application == nullptr
            ? nullptr
            : env->CallStaticObjectMethod(activity_thread, current_application);
        jclass application_class = application == nullptr
            ? nullptr
            : env->GetObjectClass(application);
        jmethodID get_loader = application_class == nullptr
            ? nullptr
            : env->GetMethodID(
                  application_class,
                  "getClassLoader",
                  "()Ljava/lang/ClassLoader;");
        jobject loader = get_loader == nullptr
            ? nullptr
            : env->CallObjectMethod(application, get_loader);
        if (env->ExceptionCheck() || loader == nullptr) {
            env->ExceptionClear();
            LOGE("unable to resolve app ClassLoader after handleBindApplication");
        } else {
            record->bootstrap_callback(env, loader, record->bootstrap_context);
        }
        return result;
    }
    if (!record->active.load(std::memory_order_acquire) ||
        (record->before == nullptr && record->after == nullptr) || g_inside_native_hook) {
        return call_original(env, record->backup, arguments, record->is_static);
    }

    const jsize argument_count = arguments == nullptr ? 0 : env->GetArrayLength(arguments);
    const unsigned int argument_offset = record->is_static ? 0U : 1U;
    const size_t expected_count = record->parsed_signature.parameters.size() + argument_offset;
    if (argument_count < 0 || static_cast<size_t>(argument_count) != expected_count) {
        LOGE(
            "hook %llu received %d bridge arguments, expected %zu",
            record->id,
            argument_count,
            expected_count);
        return call_original(env, record->backup, arguments, record->is_static);
    }
    jobjectArray working_arguments = dejavu_hook_copy_arguments(env, arguments);
    if (working_arguments == nullptr || env->ExceptionCheck()) {
        return nullptr;
    }

    dejavu_hook_context context;
    context.env = env;
    context.id = record->id;
    context.arguments = working_arguments;
    context.argument_offset = argument_offset;
    context.this_object = record->is_static
        ? nullptr
        : env->GetObjectArrayElement(working_arguments, 0);
    context.class_loader = record->class_loader;
    context.signature = &record->parsed_signature;
    context.jni = &impl_->hook_jni_api;

    g_inside_native_hook = true;
#if DEJAVU_RUN_DEVICE_STRESS
    const bool track_stress =
        impl_->stress_hook_id.load(std::memory_order_acquire) == record->id;
    if (track_stress) {
        impl_->stress_calls.fetch_add(1, std::memory_order_relaxed);
        const unsigned int in_flight =
            impl_->stress_in_flight.fetch_add(1, std::memory_order_acq_rel) + 1;
        unsigned int maximum = impl_->stress_max_parallel.load(std::memory_order_relaxed);
        while (maximum < in_flight &&
               !impl_->stress_max_parallel.compare_exchange_weak(
                   maximum, in_flight, std::memory_order_release, std::memory_order_relaxed)) {
        }
    }
#endif
    jobject result = nullptr;
    int callback_status = DEJAVU_HOOK_OK;
    if (record->before != nullptr) {
        callback_status = record->before(&context);
        if (callback_status == DEJAVU_HOOK_OK) {
            callback_status = context.helper_status;
        }
    }
    if (callback_status != DEJAVU_HOOK_OK) {
        LOGE("hook %llu before_hook failed: %d; calling original", record->id, callback_status);
        if (context.result_owned && context.result != nullptr) {
            env->DeleteLocalRef(context.result);
        }
        context.result = nullptr;
        context.result_owned = false;
        context.result_set = false;
        result = call_original(env, record->backup, arguments, record->is_static);
    } else {
        if (!context.result_set) {
            context.result = call_original(
                env, record->backup, working_arguments, record->is_static);
            context.result_owned = context.result != nullptr;
        }
        if (!env->ExceptionCheck() && record->after != nullptr) {
            jobject fallback = context.result == nullptr
                ? nullptr
                : env->NewLocalRef(context.result);
            if (!env->ExceptionCheck()) {
                context.phase = DEJAVU_HOOK_PHASE_AFTER;
                context.helper_status = DEJAVU_HOOK_OK;
                callback_status = record->after(&context);
                if (callback_status == DEJAVU_HOOK_OK) {
                    callback_status = context.helper_status;
                }
                if (callback_status != DEJAVU_HOOK_OK) {
                    LOGE(
                        "hook %llu after_hook failed: %d; preserving previous result",
                        record->id,
                        callback_status);
                    if (context.result_owned && context.result != nullptr) {
                        env->DeleteLocalRef(context.result);
                    }
                    context.result = fallback;
                    context.result_owned = fallback != nullptr;
                } else if (fallback != nullptr) {
                    env->DeleteLocalRef(fallback);
                }
            }
        }
        result = context.result;
        context.result_owned = false;
    }
#if DEJAVU_RUN_DEVICE_STRESS
    if (track_stress) {
        impl_->stress_in_flight.fetch_sub(1, std::memory_order_acq_rel);
    }
#endif
    g_inside_native_hook = false;
    return result;
}

jobject HookManager::call_original(
    JNIEnv *env,
    jobject backup_method,
    jobjectArray arguments,
    bool is_static) {
    if (impl_ == nullptr || env == nullptr || backup_method == nullptr) {
        return nullptr;
    }
    const jsize count = arguments == nullptr ? 0 : env->GetArrayLength(arguments);
    const jsize offset = is_static ? 0 : 1;
    jobject receiver = offset == 0 || count == 0
        ? nullptr
        : env->GetObjectArrayElement(arguments, 0);
    jclass object_class = env->FindClass("java/lang/Object");
    jobjectArray parameters = env->NewObjectArray(
        count >= offset ? count - offset : 0, object_class, nullptr);
    for (jsize index = offset; index < count; ++index) {
        env->SetObjectArrayElement(
            parameters, index - offset, env->GetObjectArrayElement(arguments, index));
    }
    return env->CallObjectMethod(
        backup_method, impl_->method_invoke, receiver, parameters);
}


