#pragma once

#include <jni.h>

#include "lsplant_backend.h"

enum class LsplantStatus {
    kUnavailable,
    kReady,
    kFailed,
};

struct LsplantRuntimeResult {
    LsplantStatus status;
    bool backend_attempted;
    bool hooks_may_remain;
    const DejavuLsplantBackend *backend;
};

LsplantRuntimeResult lsplant_runtime_initialize(JNIEnv *env);
