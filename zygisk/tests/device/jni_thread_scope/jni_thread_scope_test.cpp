#include "jni_thread_scope.h"

#include <exception>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace {
void require(bool condition) {
    if (!condition)
        throw std::runtime_error("JNI attachment lifecycle assertion failed");
}

void require_detached(JavaVM *vm) {
    JNIEnv *env = nullptr;
    require(vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_EDETACHED);
}

void invoke_java(JNIEnv *env) {
    jclass process = env->FindClass("android/os/Process");
    require(process != nullptr && !env->ExceptionCheck());
    jmethodID my_pid = env->GetStaticMethodID(process, "myPid", "()I");
    require(my_pid != nullptr && !env->ExceptionCheck());
    jint pid = env->CallStaticIntMethod(process, my_pid);
    env->DeleteLocalRef(process);
    require(!env->ExceptionCheck() && pid == getpid());
}
} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_dejavu_tests_JniThreadScopeInstrumentation_runScopeChecks(JNIEnv *env, jclass) {
    try {
        JavaVM *vm = nullptr;
        require(env->GetJavaVM(&vm) == JNI_OK && vm != nullptr);
        {
            JniThreadScope borrowed(vm);
            require(borrowed.valid() && borrowed.env() == env);
            invoke_java(borrowed.env());
        }
        JNIEnv *after_borrow = nullptr;
        require(vm->GetEnv(reinterpret_cast<void **>(&after_borrow), JNI_VERSION_1_6) == JNI_OK);
        require(after_borrow == env);
        std::exception_ptr failure;
        std::thread worker([&] {
            try {
                for (int iteration = 0; iteration < 32; ++iteration) {
                    require_detached(vm);
                    {
                        JniThreadScope owner(vm);
                        require(owner.valid());
                        invoke_java(owner.env());
                        {
                            JniThreadScope nested(vm);
                            require(nested.valid() && nested.env() == owner.env());
                        }
                        JNIEnv *still_attached = nullptr;
                        require(vm->GetEnv(reinterpret_cast<void **>(&still_attached),
                                           JNI_VERSION_1_6) == JNI_OK);
                    }
                    require_detached(vm);
                    try {
                        JniThreadScope throwing(vm);
                        require(throwing.valid());
                        invoke_java(throwing.env());
                        throw iteration;
                    } catch (int value) {
                        require(value == iteration);
                    }
                    require_detached(vm);
                }
            } catch (...) {
                failure = std::current_exception();
            }
        });
        worker.join();
        if (failure)
            std::rethrow_exception(failure);
        return env->NewStringUTF("PASS: real ART borrowed attachments, 64 owned attach/detach "
                                 "pairs, nested scopes, exception cleanup and native thread exit");
    } catch (...) {
        jclass error = env->FindClass("java/lang/AssertionError");
        if (error)
            env->ThrowNew(error, "JNI thread scope regression failed");
        return nullptr;
    }
}
