#pragma once

#include <jni.h>

#include <array>
#include <string>

#include "dejavu_hook_api.h"
#include "dejavu_runtime.h"
#include "hook_signature.h"

struct DejavuPrimitiveApi {
    jclass type = nullptr;
    jmethodID unbox = nullptr;
    jmethodID box = nullptr;
};

struct DejavuHookJniApi {
    std::array<DejavuPrimitiveApi, 8> primitives;
};

struct dejavu_hook_context {
    JNIEnv *env = nullptr;
    unsigned long long id = 0;
    unsigned int phase = DEJAVU_HOOK_PHASE_BEFORE;
    jobjectArray arguments = nullptr;
    unsigned int argument_offset = 0;
    jobject this_object = nullptr;
    jobject class_loader = nullptr;
    const DejavuHookSignature *signature = nullptr;
    DejavuHookJniApi *jni = nullptr;
    jobject result = nullptr;
    bool result_owned = false;
    bool result_set = false;
    int helper_status = DEJAVU_HOOK_OK;
};

const char *dejavu_hook_api_preamble();
bool dejavu_hook_api_initialize(
    JNIEnv *env,
    DejavuHookJniApi *api,
    std::string *error);
bool dejavu_hook_api_register_symbols(
    DejavuRuntime *runtime,
    dejavu_engine *engine,
    std::string *error);
jobjectArray dejavu_hook_copy_arguments(JNIEnv *env, jobjectArray arguments);
