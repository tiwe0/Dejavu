#pragma once

#include <stddef.h>
#include <stdint.h>

#include <jni.h>

#include "control_channel.h"

#define DEJAVU_AGENT_ABI_VERSION 3u

struct DejavuAgentCreateArgs {
    uint32_t struct_size;
    uint32_t abi_version;
    JNIEnv *env;
    JavaVM *vm;
    int module_dir_fd;
    DejavuControlEndpoint control_endpoint;
    const char *process_name;
};

struct DejavuAgentApi {
    uint32_t struct_size;
    uint32_t abi_version;
    void *(*create)(const DejavuAgentCreateArgs *args);
    void (*post_app_specialize)(void *instance);
};

using DejavuAgentGetApi = const DejavuAgentApi *(*)(uint32_t abi_version);

extern "C" __attribute__((visibility("default")))
const DejavuAgentApi *dejavu_agent_get_api(uint32_t abi_version);
