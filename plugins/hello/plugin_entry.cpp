#include "dejavu_plugin.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

struct HelloState {
    const DejavuPluginHost *host = nullptr;
    const DejavuMemInterface *mem = nullptr;
    const DejavuModuleInterface *module = nullptr;
    const DejavuHookInterface *hook = nullptr;
};

char *make_string(const HelloState *state, const std::string &value) {
    if (state == nullptr || state->host == nullptr || state->host->alloc == nullptr) {
        return nullptr;
    }
    char *copy = static_cast<char *>(state->host->alloc(value.size() + 1));
    if (copy == nullptr) {
        return nullptr;
    }
    memcpy(copy, value.data(), value.size());
    copy[value.size()] = '\0';
    return copy;
}

int lua_greet(dejavu_control_session *, void *context, const dejavu_value *, size_t,
              dejavu_value *result, char *error, size_t error_size) {
    auto *state = static_cast<HelloState *>(context);
    const std::string message = "hello from the Dejavu plugin ABI";
    char *copy = make_string(state, message);
    if (copy == nullptr) {
        std::snprintf(error, error_size, "unable to allocate greeting");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    result->type = DEJAVU_VALUE_STRING;
    result->owned = 1;
    result->string_value = copy;
    result->string_size = message.size();
    return DEJAVU_OK;
}

int lua_info(dejavu_control_session *, void *context, const dejavu_value *, size_t,
             dejavu_value *result, char *error, size_t error_size) {
    auto *state = static_cast<HelloState *>(context);
    if (state == nullptr || state->mem == nullptr) {
        std::snprintf(error, error_size, "host mem interface is unavailable");
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    static int marker = 0;
    uintptr_t start = 0;
    uintptr_t end = 0;
    uint32_t prot = 0;
    const int query = state->mem->query(state->host->host_context,
                                        reinterpret_cast<uintptr_t>(&marker), &start, &end, &prot);
    std::string text = "hello 1.0.0";
    text += " interfaces=mem";
    text += state->module != nullptr ? ",module" : "";
    text += state->hook != nullptr ? ",hook" : "";
    text += query == 0 ? " query=ok" : " query=failed";
    if (state->module != nullptr) {
        void *getpid_symbol =
            state->module->resolve(state->host->host_context, "libc.so", "getpid");
        text += getpid_symbol != nullptr ? " resolve=ok" : " resolve=failed";
    }
    char *copy = make_string(state, text);
    if (copy == nullptr) {
        std::snprintf(error, error_size, "unable to allocate info");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    result->type = DEJAVU_VALUE_STRING;
    result->owned = 1;
    result->string_value = copy;
    result->string_size = text.size();
    return DEJAVU_OK;
}

int plugin_initialize(const DejavuPluginHost *host, void **plugin_state, char *error,
                      size_t error_size) {
    if (host == nullptr || plugin_state == nullptr ||
        host->abi_version != DEJAVU_PLUGIN_ABI_VERSION ||
        host->struct_size <
            offsetof(DejavuPluginHost, get_interface) + sizeof(host->get_interface)) {
        std::snprintf(error, error_size, "incompatible plugin host ABI");
        return -1;
    }
    auto *state = new HelloState();
    state->host = host;
    if (host->get_interface != nullptr) {
        state->mem = static_cast<const DejavuMemInterface *>(
            host->get_interface(host->host_context, "mem", DEJAVU_MEM_INTERFACE_VERSION));
        state->module = static_cast<const DejavuModuleInterface *>(
            host->get_interface(host->host_context, "module", DEJAVU_MODULE_INTERFACE_VERSION));
        state->hook = static_cast<const DejavuHookInterface *>(
            host->get_interface(host->host_context, "hook", DEJAVU_HOOK_INTERFACE_VERSION));
    }
    struct Registration {
        const char *name;
        dejavu_host_function function;
    };
    const Registration registrations[] = {
        {"greet", lua_greet},
        {"info", lua_info},
    };
    for (const Registration &registration : registrations) {
        const int status = host->register_function(host->host_context, "hello", registration.name,
                                                   registration.function, state, error, error_size);
        if (status != DEJAVU_OK) {
            delete state;
            return -1;
        }
    }
    *plugin_state = state;
    return 0;
}

void plugin_control_ready(void *plugin_state) {
    auto *state = static_cast<HelloState *>(plugin_state);
    if (state != nullptr && state->host != nullptr && state->host->log_message != nullptr) {
        state->host->log_message(state->host->host_context, 4,
                                 "hello plugin: control session ready");
    }
}

void plugin_shutdown(void *plugin_state) {
    delete static_cast<HelloState *>(plugin_state);
}

} // namespace

extern "C" __attribute__((visibility("default"))) const DejavuPluginDescriptor *
dejavu_plugin_entry(void) {
    static DejavuPluginDescriptor descriptor = {
        DEJAVU_PLUGIN_ABI_VERSION,
        sizeof(DejavuPluginDescriptor),
        "hello",
        "1.0.0",
        DEJAVU_PLUGIN_FLAG_UNLOAD_SAFE,
        plugin_initialize,
        plugin_control_ready,
        plugin_shutdown,
        DEJAVU_PLUGIN_CAP_MEM | DEJAVU_PLUGIN_CAP_MODULE | DEJAVU_PLUGIN_CAP_HOOK,
        nullptr,
    };
    return &descriptor;
}
