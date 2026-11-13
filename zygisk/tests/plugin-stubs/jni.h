#pragma once

#include <atomic>

using jobject = void *;
constexpr int JNI_OK = 0;
constexpr int JNI_ERR = -1;
constexpr int JNI_EDETACHED = -2;
constexpr int JNI_EVERSION = -3;
constexpr int JNI_VERSION_1_6 = 0x00010006;
struct JNIEnv {
    void DeleteGlobalRef(jobject) {
    }
    jobject NewGlobalRef(jobject value) {
        return value;
    }
};
struct JavaVM {
    // Default preserves the previous unavailable-JVM fixture behavior.
    int get_env_result = JNI_ERR;
    int attach_result = JNI_OK;
    bool null_env = false;
    std::atomic<int> attaches{0};
    std::atomic<int> detaches{0};
    inline static thread_local JavaVM *attached_vm = nullptr;
    JNIEnv env;

    int GetEnv(void **result, int) {
        int status = get_env_result;
        if (status == JNI_EDETACHED && attached_vm == this)
            status = JNI_OK;
        if (result != nullptr)
            *result = status == JNI_OK && !null_env ? &env : nullptr;
        return status;
    }
    int AttachCurrentThread(JNIEnv **result, void *) {
        ++attaches;
        if (attach_result == JNI_OK)
            attached_vm = this;
        if (result != nullptr)
            *result = attach_result == JNI_OK && !null_env ? &env : nullptr;
        return attach_result;
    }
    int DetachCurrentThread() {
        ++detaches;
        if (attached_vm == this)
            attached_vm = nullptr;
        return JNI_OK;
    }
};
