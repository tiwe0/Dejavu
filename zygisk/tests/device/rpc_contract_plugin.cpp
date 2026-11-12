#include "dejavu_plugin.h"
#include <sys/system_properties.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <thread>

namespace {
struct State {
    const DejavuPluginHost *host;
    const DejavuRpcInterface *rpc;
    uint64_t handle = 0;
    std::atomic<unsigned> entered{0}, finished{0}, cancelled{0}, active{0};
};
int reply(State *state, DejavuRpcResponse *response, const char *text, size_t size) {
    char *value = static_cast<char *>(state->host->alloc(size + 1));
    if (!value)
        return DEJAVU_RPC_ERROR;
    std::memcpy(value, text, size);
    value[size] = 0;
    response->ok = 1;
    response->payload_json = value;
    response->payload_size = size;
    return DEJAVU_RPC_OK;
}
int echo(void *opaque, const DejavuRpcRequest *request, DejavuRpcResponse *response, char *,
         size_t) {
    return reply(static_cast<State *>(opaque), response, request->params_json,
                 request->params_size);
}
int work(void *opaque, const DejavuRpcRequest *request, DejavuRpcResponse *response, char *,
         size_t) {
    auto *state = static_cast<State *>(opaque);
    ++state->entered;
    ++state->active;
    const bool cooperate = std::strcmp(request->method, "wait") == 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1300);
    while (std::chrono::steady_clock::now() < until) {
        if (cooperate && request->is_cancelled(request->cancellation_context)) {
            ++state->cancelled;
            --state->active;
            return DEJAVU_RPC_ERROR;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    --state->active;
    ++state->finished;
    const char text[] = "{\"source\":\"old-business-callback\"}";
    return reply(state, response, text, sizeof(text) - 1);
}
int status(dejavu_control_session *, void *opaque, const dejavu_value *, size_t,
           dejavu_value *result, char *, size_t) {
    auto *state = static_cast<State *>(opaque);
    char buffer[180];
    const int size = std::snprintf(
        buffer, sizeof(buffer), "{\"entered\":%u,\"finished\":%u,\"cancelled\":%u,\"active\":%u}",
        state->entered.load(), state->finished.load(), state->cancelled.load(),
        state->active.load());
    char *copy = static_cast<char *>(state->host->alloc(size + 1));
    if (!copy)
        return DEJAVU_ERROR_LUA_EXECUTE;
    std::memcpy(copy, buffer, size + 1);
    *result = {};
    result->type = DEJAVU_VALUE_STRING;
    result->owned = 1;
    result->string_value = copy;
    result->string_size = size;
    return DEJAVU_OK;
}
int initialize(const DejavuPluginHost *host, void **output, char *error, size_t capacity) {
    *output = nullptr;
    const auto *rpc =
        static_cast<const DejavuRpcInterface *>(host->get_interface(host->host_context, "rpc", 1));
    if (!rpc || rpc->version != 1 || rpc->struct_size < sizeof(*rpc))
        return DEJAVU_RPC_ERROR;
    char endpoint[PROP_VALUE_MAX]{};
    if (__system_property_get("debug.dejavu.rpc.fixture", endpoint) <= 0)
        return DEJAVU_RPC_ERROR;
    auto *state = new (std::nothrow) State{host, rpc};
    if (!state)
        return DEJAVU_RPC_ERROR;
    const std::string address = std::string("abstract:") + endpoint;
    const char *routes[] = {"dejavu.test.rpc"};
    const DejavuRpcMethod methods[] = {{"echo", echo}, {"wait", work}, {"ignore", work}};
    const DejavuRpcServiceDescriptor service{
        sizeof(service), "rpc_contract_test", "dejavu.rpc.fixture", routes, 1, methods, 3,
        state,           address.c_str()};
    if (rpc->register_service(host->host_context, &service, &state->handle, error, capacity) != 0) {
        delete state;
        return DEJAVU_RPC_ERROR;
    }
    if (host->register_function(host->host_context, "rpc_fixture", "status", status, state, error,
                                capacity) != DEJAVU_OK) {
        rpc->unregister_service(host->host_context, state->handle, error, capacity);
        delete state;
        return DEJAVU_RPC_ERROR;
    }
    *output = state;
    return DEJAVU_RPC_OK;
}
void prepare(void *opaque) {
    auto *state = static_cast<State *>(opaque);
    if (state && state->handle) {
        char error[128]{};
        state->rpc->unregister_service(state->host->host_context, state->handle, error,
                                       sizeof(error));
        state->handle = 0;
    }
}
void shutdown(void *opaque) {
    prepare(opaque);
    delete static_cast<State *>(opaque);
}
} // namespace
extern "C" __attribute__((visibility("default"))) const DejavuPluginDescriptor *
dejavu_plugin_entry() {
    static const DejavuPluginDescriptor descriptor{DEJAVU_PLUGIN_ABI_VERSION,
                                                   sizeof(descriptor),
                                                   "rpc_contract_test",
                                                   "1.0.0",
                                                   DEJAVU_PLUGIN_FLAG_UNLOAD_SAFE,
                                                   initialize,
                                                   nullptr,
                                                   shutdown,
                                                   DEJAVU_PLUGIN_CAP_RPC,
                                                   prepare};
    return &descriptor;
}
