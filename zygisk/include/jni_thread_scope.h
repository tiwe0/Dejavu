#pragma once

#include <jni.h>

// Internal callback scope for finite native threads. Existing JNI attachments
// are borrowed; only an attachment acquired by this scope is detached.
class JniThreadScope {
  public:
    explicit JniThreadScope(JavaVM *vm) : vm_(vm) {
        if (vm_ == nullptr)
            return;
        const int status = vm_->GetEnv(reinterpret_cast<void **>(&env_), JNI_VERSION_1_6);
        if (status == JNI_EDETACHED) {
            env_ = nullptr;
            owned_ = vm_->AttachCurrentThread(&env_, nullptr) == JNI_OK;
            if (!owned_)
                env_ = nullptr;
        } else if (status != JNI_OK) {
            env_ = nullptr;
        }
    }
    ~JniThreadScope() noexcept {
        if (owned_)
            vm_->DetachCurrentThread();
    }
    JniThreadScope(const JniThreadScope &) = delete;
    JniThreadScope &operator=(const JniThreadScope &) = delete;
    bool valid() const noexcept {
        return env_ != nullptr;
    }
    JNIEnv *env() const noexcept {
        return env_;
    }

  private:
    JavaVM *vm_;
    JNIEnv *env_ = nullptr;
    bool owned_ = false;
};
