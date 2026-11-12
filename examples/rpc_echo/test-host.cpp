#include "dejavu_plugin.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>
#include <cstdio>

static DejavuRpcServiceDescriptor captured{};
static int allocation_count = 0;
static int unregister_count = 0;
static bool available = true;
static bool registration_ok = true;
static void *allocate(size_t size) { ++allocation_count; return std::malloc(size); }
static void release(void *value) { if (value) --allocation_count; std::free(value); }
static int register_service(void *, const DejavuRpcServiceDescriptor *value, uint64_t *handle, char *, size_t) {
    if (!registration_ok) return DEJAVU_RPC_ERROR;
    captured = *value;
    *handle = 42;
    return DEJAVU_RPC_OK;
}
static int unregister_service(void *, uint64_t handle, char *, size_t) {
    assert(handle == 42); ++unregister_count; return DEJAVU_RPC_OK;
}
static const DejavuRpcInterface rpc = {DEJAVU_RPC_INTERFACE_VERSION, sizeof(rpc), register_service, nullptr, unregister_service};
static const void *lookup(void *, const char *name, uint32_t version) {
    assert(std::string(name) == "rpc" && version == DEJAVU_RPC_INTERFACE_VERSION);
    return available ? &rpc : nullptr;
}
static int is_cancelled(void *) { return 1; }
int main() {
    const auto *plugin = dejavu_plugin_entry();
    assert(plugin->capabilities == DEJAVU_PLUGIN_CAP_RPC);
    assert(plugin->abi_version == DEJAVU_PLUGIN_ABI_VERSION);
    DejavuPluginHost host{};
    host.abi_version = DEJAVU_PLUGIN_ABI_VERSION;
    host.struct_size = sizeof(host);
    host.alloc = allocate; host.free = release; host.get_interface = lookup;
    char error[256] = {};
    void *state = reinterpret_cast<void *>(1);
    available = false;
    assert(plugin->initialize(&host, &state, error, sizeof(error)) == DEJAVU_RPC_ERROR && !state);
    available = true; registration_ok = false;
    assert(plugin->initialize(&host, &state, error, sizeof(error)) == DEJAVU_RPC_ERROR && !state);
    registration_ok = true;
    assert(plugin->initialize(&host, &state, error, sizeof(error)) == DEJAVU_RPC_OK && state);
    assert(captured.route_count == 1 && std::string(captured.routes[0]) == "example.echo");
    assert(captured.method_count == 3 && captured.endpoint == nullptr);
    const char params[] = "{\"value\":\"hello\"}";
    DejavuRpcRequest request{sizeof(request), "test", "example.echo", "echo", params, sizeof(params) - 1, 0, nullptr, nullptr};
    DejavuRpcResponse response{};
    assert(captured.methods[0].callback(captured.context, &request, &response, error, sizeof(error)) == DEJAVU_RPC_OK);
    assert(response.ok == 1 && response.struct_size == sizeof(response));
    assert(response.payload_size == sizeof(params) - 1 && std::string(response.payload_json) == params);
    release(response.payload_json); response = {};
    assert(captured.methods[1].callback(captured.context, &request, &response, error, sizeof(error)) == DEJAVU_RPC_OK);
    assert(response.ok == 1 && std::string(response.payload_json).find("rpc_echo") != std::string::npos);
    release(response.payload_json); response = {};
    assert(captured.methods[2].callback(captured.context, &request, &response, error, sizeof(error)) == DEJAVU_RPC_OK);
    assert(response.ok == 0 && std::string(response.payload_json).find("example.rejected") != std::string::npos);
    release(response.payload_json); response = {};
    request.is_cancelled = is_cancelled;
    assert(captured.methods[0].callback(captured.context, &request, &response, error, sizeof(error)) == DEJAVU_RPC_ERROR);
    assert(response.payload_json == nullptr && allocation_count == 0);
    plugin->on_unload_prepare(state);
    plugin->shutdown(state);
    assert(unregister_count == 1 && allocation_count == 0);
    std::puts("PASS: RPC example ABI, missing-interface/registration failure, method table, JSON allocation ownership, cancellation and unload");
}
