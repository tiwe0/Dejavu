#include "dejavu_plugin.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int fake_ssl_write(void *, const void *, int len) { return len; }
int fake_ssl_read(void *, void *buf, int len) {
    const char *msg = "HTTP/1.1 200 OK";
    const int n = static_cast<int>(std::strlen(msg));
    const int copy = len < n ? len : n;
    std::memcpy(buf, msg, static_cast<size_t>(copy));
    return copy;
}

using WriteFn = int (*)(void *, const void *, int);
using ReadFn = int (*)(void *, void *, int);

WriteFn g_hooked_write = nullptr;
ReadFn g_hooked_read = nullptr;
int g_alloc_count = 0;
int g_hook_installs = 0;
int g_hook_uninstalls = 0;
int g_stream_writes = 0;
int g_rpc_unregisters = 0;
bool g_rpc_available = true;
DejavuRpcServiceDescriptor g_rpc_desc{};
std::vector<DejavuRpcMethod> g_rpc_methods;
std::string g_rpc_route;

void *host_alloc(size_t size) {
    ++g_alloc_count;
    return std::malloc(size);
}
void host_free(void *ptr) {
    if (ptr != nullptr) {
        --g_alloc_count;
        std::free(ptr);
    }
}
int host_register_fn(void *, const char *, const char *, dejavu_host_function, void *, char *,
                     size_t) {
    return DEJAVU_OK;
}
void *mod_resolve(void *, const char *lib, const char *sym) {
    if (std::string(lib) == "libssl.so") {
        if (std::string(sym) == "SSL_write") return reinterpret_cast<void *>(&fake_ssl_write);
        if (std::string(sym) == "SSL_read") return reinterpret_cast<void *>(&fake_ssl_read);
    }
    return nullptr;
}
int hook_install(void *, void *target, void *replacement, void **original, char *, size_t) {
    ++g_hook_installs;
    *original = target;
    if (target == reinterpret_cast<void *>(&fake_ssl_write)) {
        g_hooked_write = reinterpret_cast<WriteFn>(replacement);
    } else if (target == reinterpret_cast<void *>(&fake_ssl_read)) {
        g_hooked_read = reinterpret_cast<ReadFn>(replacement);
    }
    return 0;
}
int hook_uninstall(void *, void *, char *, size_t) {
    ++g_hook_uninstalls;
    return 0;
}
int stream_open(void *, uint32_t, const char *, char *, size_t) { return 7; }
int stream_write(void *, int, const void *, size_t) {
    ++g_stream_writes;
    return 0;
}
int stream_close(void *, int) { return 0; }
int rpc_register(void *, const DejavuRpcServiceDescriptor *desc, uint64_t *handle, char *, size_t) {
    g_rpc_desc = *desc;
    g_rpc_methods.assign(desc->methods, desc->methods + desc->method_count);
    g_rpc_route = desc->route_count > 0 ? desc->routes[0] : "";
    *handle = 101;
    return DEJAVU_RPC_OK;
}
int rpc_unregister(void *, uint64_t handle, char *, size_t) {
    assert(handle == 101);
    ++g_rpc_unregisters;
    return DEJAVU_RPC_OK;
}

const DejavuModuleInterface g_mod_iface = {DEJAVU_MODULE_INTERFACE_VERSION, sizeof(g_mod_iface),
                                           mod_resolve};
const DejavuHookInterface g_hook_iface = {DEJAVU_HOOK_INTERFACE_VERSION, sizeof(g_hook_iface),
                                          hook_install, hook_uninstall};
const DejavuStreamInterface g_stream_iface = {DEJAVU_STREAM_INTERFACE_VERSION,
                                              sizeof(g_stream_iface), stream_open, stream_write,
                                              stream_close};
const DejavuRpcInterface g_rpc_iface = {DEJAVU_RPC_INTERFACE_VERSION, sizeof(g_rpc_iface),
                                        rpc_register, nullptr, rpc_unregister};

const void *host_get_interface(void *, const char *name, uint32_t version) {
    if (std::string(name) == "module" && version == DEJAVU_MODULE_INTERFACE_VERSION)
        return &g_mod_iface;
    if (std::string(name) == "hook" && version == DEJAVU_HOOK_INTERFACE_VERSION)
        return &g_hook_iface;
    if (std::string(name) == "stream" && version == DEJAVU_STREAM_INTERFACE_VERSION)
        return &g_stream_iface;
    if (std::string(name) == "rpc" && version == DEJAVU_RPC_INTERFACE_VERSION)
        return g_rpc_available ? &g_rpc_iface : nullptr;
    return nullptr;
}

DejavuRpcHandler find_method(const char *name) {
    for (const auto &m : g_rpc_methods) {
        if (std::string(m.name) == name) return m.callback;
    }
    return nullptr;
}

std::string call_rpc(const char *method, const char *params, int *ok_out) {
    auto handler = find_method(method);
    assert(handler != nullptr);
    DejavuRpcRequest req{sizeof(req), "req-1", g_rpc_route.c_str(), method,
                         params,      std::strlen(params), 0, nullptr, nullptr};
    DejavuRpcResponse resp{};
    char err[256] = {};
    assert(handler(g_rpc_desc.context, &req, &resp, err, sizeof(err)) == DEJAVU_RPC_OK);
    *ok_out = resp.ok;
    std::string body(resp.payload_json, resp.payload_size);
    host_free(resp.payload_json);
    return body;
}

} // namespace

int main() {
    const auto *plugin = dejavu_plugin_entry();
    assert((plugin->capabilities & DEJAVU_PLUGIN_CAP_RPC) != 0);
    assert((plugin->flags & DEJAVU_PLUGIN_FLAG_UNLOAD_SAFE) != 0);

    DejavuPluginHost host{};
    host.abi_version = DEJAVU_PLUGIN_ABI_VERSION;
    host.struct_size = sizeof(host);
    host.register_function = host_register_fn;
    host.alloc = host_alloc;
    host.free = host_free;
    host.get_interface = host_get_interface;

    // 1. Fails cleanly when RPC v1 interface is missing.
    char err[256] = {};
    void *state = reinterpret_cast<void *>(1);
    g_rpc_available = false;
    assert(plugin->initialize(&host, &state, err, sizeof(err)) != 0 && state == nullptr);
    assert(g_rpc_unregisters == 0 && g_alloc_count == 0);

    // 2. Full Router RPC + memory-only capture + stream capture.
    g_rpc_available = true;
    state = nullptr;
    assert(plugin->initialize(&host, &state, err, sizeof(err)) == 0 && state != nullptr);
    assert(g_rpc_route.rfind("capture.", 0) == 0);

    int ok = 0;
    std::string body = call_rpc("status", "{}", &ok);
    assert(ok == 1 && body.find("\"active\":false") != std::string::npos);

    // Memory-only + stream without file sink (previously broken when fd < 0!).
    body = call_rpc("start", "{\"transport\":1,\"endpoint\":\"127.0.0.1:39001\"}", &ok);
    assert(ok == 1 && body.find("\"active\":true") != std::string::npos);
    assert(g_hook_installs == 2 && g_hooked_write != nullptr && g_hooked_read != nullptr);

    const char *req_data = "GET /api/v1/health HTTP/1.1\r\n";
    assert(g_hooked_write(nullptr, req_data, static_cast<int>(std::strlen(req_data))) > 0);
    char resp_buf[64] = {};
    assert(g_hooked_read(nullptr, resp_buf, sizeof(resp_buf)) > 0);
    assert(g_stream_writes == 2);

    body = call_rpc("recent", "{\"limit\":10,\"clear\":true}", &ok);
    assert(ok == 1);
    assert(body.find("GET /api/v1/health") != std::string::npos);
    assert(body.find("HTTP/1.1 200 OK") != std::string::npos);

    body = call_rpc("status", "{}", &ok);
    assert(ok == 1 && body.find("\"write_calls\":1") != std::string::npos &&
           body.find("\"read_calls\":1") != std::string::npos &&
           body.find("\"buffered_records\":0") != std::string::npos);

    body = call_rpc("stop", "{}", &ok);
    assert(ok == 1 && body.find("\"active\":false") != std::string::npos);
    assert(g_hook_uninstalls == 2);

    plugin->on_unload_prepare(state);
    plugin->shutdown(state);
    assert(g_rpc_unregisters == 1 && g_alloc_count == 0);
    std::puts("PASS: capture Router RPC, stream+memory ring buffer, and safe unload");
    return 0;
}
