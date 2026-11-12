#include "dejavu_plugin.h"

#include <cstdio>
#include <cstring>
#include <new>

namespace {
struct State {
    const DejavuPluginHost *host;
    const DejavuRpcInterface *rpc;
    uint64_t service = 0;
};

int reply(State *state, DejavuRpcResponse *response, bool ok, const char *json, size_t size,
          char *error, size_t error_size) {
    auto *copy = static_cast<char *>(state->host->alloc(size + 1));
    if (!copy) {
        std::snprintf(error, error_size, "unable to allocate response");
        return DEJAVU_RPC_ERROR;
    }
    std::memcpy(copy, json, size);
    copy[size] = '\0';
    response->struct_size = sizeof(*response);
    response->ok = ok ? 1 : 0;
    response->payload_json = copy;
    response->payload_size = size;
    return DEJAVU_RPC_OK;
}

int echo(void *context, const DejavuRpcRequest *request, DejavuRpcResponse *response,
         char *error, size_t error_size) {
    if (request->is_cancelled && request->is_cancelled(request->cancellation_context)) {
        std::snprintf(error, error_size, "request cancelled");
        return DEJAVU_RPC_ERROR;
    }
    if (!request->params_json || !request->params_size) {
        std::snprintf(error, error_size, "params missing");
        return DEJAVU_RPC_ERROR;
    }
    return reply(static_cast<State *>(context), response, true, request->params_json,
                 request->params_size, error, error_size);
}

int status(void *context, const DejavuRpcRequest *, DejavuRpcResponse *response,
           char *error, size_t error_size) {
    static const char json[] = "{\"service\":\"rpc_echo\",\"version\":1}";
    return reply(static_cast<State *>(context), response, true, json, sizeof(json) - 1, error, error_size);
}

int fail(void *context, const DejavuRpcRequest *, DejavuRpcResponse *response,
         char *error, size_t error_size) {
    static const char json[] = "{\"code\":\"example.rejected\",\"message\":\"Example business rejection\",\"retryable\":false}";
    return reply(static_cast<State *>(context), response, false, json, sizeof(json) - 1, error, error_size);
}

int initialize(const DejavuPluginHost *host, void **plugin_state, char *error, size_t error_size) {
    if (plugin_state) *plugin_state = nullptr;
    if (!host || !plugin_state || host->abi_version != DEJAVU_PLUGIN_ABI_VERSION ||
        host->struct_size < offsetof(DejavuPluginHost, get_interface) + sizeof(host->get_interface) ||
        !host->get_interface || !host->alloc || !host->free) {
        std::snprintf(error, error_size, "incompatible host");
        return DEJAVU_RPC_ERROR;
    }
    const auto *rpc = static_cast<const DejavuRpcInterface *>(host->get_interface(
        host->host_context, "rpc", DEJAVU_RPC_INTERFACE_VERSION));
    if (!rpc || rpc->version != DEJAVU_RPC_INTERFACE_VERSION || rpc->struct_size < sizeof(*rpc) ||
        !rpc->register_service || !rpc->unregister_service) {
        std::snprintf(error, error_size, "RPC interface v1 required");
        return DEJAVU_RPC_ERROR;
    }
    auto *state = new (std::nothrow) State{host, rpc};
    if (!state) { std::snprintf(error, error_size, "out of memory"); return DEJAVU_RPC_ERROR; }
    static const char *routes[] = {"example.echo"};
    static const DejavuRpcMethod methods[] = {{"echo", echo}, {"status", status}, {"fail", fail}};
    const DejavuRpcServiceDescriptor service = {
        sizeof(DejavuRpcServiceDescriptor), "rpc_echo", "example.rpc_echo", routes, 1,
        methods, sizeof(methods) / sizeof(methods[0]), state, nullptr,
    };
    if (rpc->register_service(host->host_context, &service, &state->service, error, error_size) != DEJAVU_RPC_OK) {
        delete state;
        return DEJAVU_RPC_ERROR;
    }
    *plugin_state = state;
    return DEJAVU_RPC_OK;
}

void prepare(void *plugin_state) {
    auto *state = static_cast<State *>(plugin_state);
    if (state && state->service) {
        char error[128] = {};
        // Host retirement drains callbacks before unload hooks are called.
        state->rpc->unregister_service(state->host->host_context, state->service, error, sizeof(error));
        state->service = 0;
    }
}
void shutdown(void *plugin_state) { prepare(plugin_state); delete static_cast<State *>(plugin_state); }
}  // namespace

extern "C" __attribute__((visibility("default"))) const DejavuPluginDescriptor *dejavu_plugin_entry(void) {
    static const DejavuPluginDescriptor descriptor = {
        DEJAVU_PLUGIN_ABI_VERSION, sizeof(DejavuPluginDescriptor), "rpc_echo", "1.0.0",
        DEJAVU_PLUGIN_FLAG_UNLOAD_SAFE, initialize, nullptr, shutdown,
        DEJAVU_PLUGIN_CAP_RPC, prepare,
    };
    return &descriptor;
}
