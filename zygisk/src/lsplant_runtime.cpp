#include "lsplant_runtime.h"

LsplantRuntimeResult lsplant_runtime_initialize(JNIEnv *env) {
#if DEJAVU_WITH_LSPLANT
    if (env == nullptr) {
        return {LsplantStatus::kFailed, false, false, nullptr};
    }
    const DejavuLsplantInitResult result = dejavu_lsplant_backend_initialize(env);
    return {
        result.status == DejavuLsplantInitStatus::kReady
            ? LsplantStatus::kReady
            : LsplantStatus::kFailed,
        result.status != DejavuLsplantInitStatus::kInvalidArgument,
        result.hooks_may_remain,
        result.backend,
    };
#else
    (void)env;
    return {LsplantStatus::kUnavailable, false, false, nullptr};
#endif
}
