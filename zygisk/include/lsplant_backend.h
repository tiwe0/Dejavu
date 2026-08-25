#pragma once

#include <stdint.h>

#include <jni.h>

enum class DejavuLsplantInitStatus : uint32_t {
    kReady = 0,
    kInvalidArgument = 1,
    kResolverFailed = 2,
    kLsplantFailed = 3,
};

struct DejavuLsplantBackend {
    uint32_t abi_version;
    jobject (*hook)(JNIEnv *env, jobject target, jobject bridge, jobject callback);
    bool (*is_hooked)(JNIEnv *env, jobject target);
    bool (*unhook)(JNIEnv *env, jobject target);
    bool (*deoptimize)(JNIEnv *env, jobject method);
};

struct DejavuLsplantInitResult {
    DejavuLsplantInitStatus status;
    bool hooks_may_remain;
    const DejavuLsplantBackend *backend;
};

/*
 * Initializes LSPlant at most once in the current process. Once this function
 * reaches lsplant::Init(), the containing Zygisk library must stay resident,
 * even when initialization reports failure.
 */
extern "C" DejavuLsplantInitResult dejavu_lsplant_backend_initialize(JNIEnv *env);
