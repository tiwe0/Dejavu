#pragma once

#include <jni.h>

#include <string>

#include "dejavu_runtime.h"
#include "lsplant_backend.h"

#define DEJAVU_HOOK_ENTRY_SYMBOL "dejavu_hook_callback"

using DejavuNativeHookCallback = void *(*)(
    void *jni_env,
    unsigned long long hook_id,
    void *backup_method,
    void *arguments,
    int is_static);

extern "C" void *dejavu_hook_call_original(
    void *jni_env,
    void *backup_method,
    void *arguments,
    int is_static);
extern "C" void dejavu_hook_log(const char *message);

class HookManager {
public:
    using BootstrapCallback = void (*)(JNIEnv *env, jobject class_loader, void *context);

    HookManager();
    ~HookManager();

    HookManager(const HookManager &) = delete;
    HookManager &operator=(const HookManager &) = delete;

    bool initialize(
        JNIEnv *env,
        JavaVM *vm,
        const DejavuLsplantBackend *backend,
        DejavuRuntime *runtime,
        std::string *error);
    bool install_app_bootstrap(
        JNIEnv *env,
        BootstrapCallback callback,
        void *context,
        std::string *error);
    bool start_control(
        JNIEnv *env,
        jobject app_class_loader,
        const std::string &script,
        std::string *error);
    bool eval_control(const std::string &script, std::string *error);

    jobject dispatch(JNIEnv *env, unsigned long long hook_id, jobjectArray arguments);
    jobject call_original(
        JNIEnv *env,
        jobject backup_method,
        jobjectArray arguments,
        bool is_static);

private:
    struct Impl;
    Impl *impl_;
};
