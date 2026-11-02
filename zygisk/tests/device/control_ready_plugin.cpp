// Device regression fixture: auto-load this plugin before the control session
// exists, then assert plugin.call("control_ready_test", "context_test.ready").
// The same check must also pass when the plugin is loaded after session setup.
#include "dejavu_plugin.h"

#include <cstdio>

namespace {

struct State {
    const DejavuPluginHost *host = nullptr;
    bool ready = false;
};

int check_ready(dejavu_control_session *, void *context, const dejavu_value *, size_t,
                dejavu_value *result, char *, size_t) {
    auto *state = static_cast<State *>(context);
    *result = {};
    result->type = DEJAVU_VALUE_BOOLEAN;
    result->boolean_value = state->ready;
    return DEJAVU_OK;
}

int initialize(const DejavuPluginHost *host, void **plugin_state, char *error, size_t error_size) {
    if (host == nullptr || host->abi_version != DEJAVU_PLUGIN_ABI_VERSION ||
        host->get_interface == nullptr || host->register_function == nullptr) {
        std::snprintf(error, error_size, "incompatible plugin host");
        return -1;
    }
    auto *state = new State{host, false};
    const int status = host->register_function(host->host_context, "context_test", "ready",
                                               check_ready, state, error, error_size);
    if (status != DEJAVU_OK) {
        delete state;
        return -1;
    }
    *plugin_state = state;
    return 0;
}

void control_ready(void *plugin_state) {
    auto *state = static_cast<State *>(plugin_state);
    state->ready = state->host->get_interface(state->host->host_context, "mem",
                                              DEJAVU_MEM_INTERFACE_VERSION) != nullptr;
}

void shutdown(void *plugin_state) {
    delete static_cast<State *>(plugin_state);
}

} // namespace

extern "C" __attribute__((visibility("default"))) const DejavuPluginDescriptor *
dejavu_plugin_entry(void) {
    static const DejavuPluginDescriptor descriptor = {
        DEJAVU_PLUGIN_ABI_VERSION,
        sizeof(DejavuPluginDescriptor),
        "control_ready_test",
        "1.0.0",
        DEJAVU_PLUGIN_FLAG_UNLOAD_SAFE,
        initialize,
        control_ready,
        shutdown,
        DEJAVU_PLUGIN_CAP_MEM,
        nullptr,
    };
    return &descriptor;
}
