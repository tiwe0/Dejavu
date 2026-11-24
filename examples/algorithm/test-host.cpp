#include "dejavu_plugin.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_fake_md_ctx = 1;
const char *fake_md_name(const void *) { return "MD5"; }
void *fake_md_ctx_new() { return &g_fake_md_ctx; }
void fake_md_ctx_free(void *) {}
const void *fake_md5() { return &g_fake_md_ctx; }

int fake_digest_init(void *, const void *, void *) { return 1; }
int fake_digest_update(void *, const void *, size_t) { return 1; }
int fake_digest_final(void *, unsigned char *md, unsigned int *size) {
    static const unsigned char kAbcMd5[16] = {
        0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0,
        0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72,
    };
    std::memcpy(md, kAbcMd5, sizeof(kAbcMd5));
    *size = sizeof(kAbcMd5);
    return 1;
}

using InitFn = int (*)(void *, const void *, void *);
using UpdateFn = int (*)(void *, const void *, size_t);
using FinalFn = int (*)(void *, unsigned char *, unsigned int *);

InitFn g_hooked_init = nullptr;
UpdateFn g_hooked_update = nullptr;
FinalFn g_hooked_final = nullptr;

int dispatch_init(void *ctx, const void *md, void *engine) {
    return g_hooked_init ? g_hooked_init(ctx, md, engine) : fake_digest_init(ctx, md, engine);
}
int dispatch_update(void *ctx, const void *data, size_t size) {
    return g_hooked_update ? g_hooked_update(ctx, data, size) : fake_digest_update(ctx, data, size);
}
int dispatch_final(void *ctx, unsigned char *md, unsigned int *size) {
    return g_hooked_final ? g_hooked_final(ctx, md, size) : fake_digest_final(ctx, md, size);
}

int g_alloc_count = 0;
int g_hook_installs = 0;
int g_hook_uninstalls = 0;
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
    if (std::string(lib) != "libcrypto.so") return nullptr;
    const std::string s(sym);
    if (s == "EVP_MD_get0_name") return reinterpret_cast<void *>(&fake_md_name);
    if (s == "EVP_MD_CTX_new") return reinterpret_cast<void *>(&fake_md_ctx_new);
    if (s == "EVP_MD_CTX_free") return reinterpret_cast<void *>(&fake_md_ctx_free);
    if (s == "EVP_md5") return reinterpret_cast<void *>(&fake_md5);
    if (s == "EVP_DigestInit_ex") return reinterpret_cast<void *>(&dispatch_init);
    if (s == "EVP_DigestUpdate") return reinterpret_cast<void *>(&dispatch_update);
    if (s == "EVP_DigestFinal_ex") return reinterpret_cast<void *>(&dispatch_final);
    return nullptr;
}
int hook_install(void *, void *target, void *replacement, void **original, char *, size_t) {
    ++g_hook_installs;
    if (target == reinterpret_cast<void *>(&dispatch_init)) {
        *original = reinterpret_cast<void *>(&fake_digest_init);
        g_hooked_init = reinterpret_cast<InitFn>(replacement);
    } else if (target == reinterpret_cast<void *>(&dispatch_update)) {
        *original = reinterpret_cast<void *>(&fake_digest_update);
        g_hooked_update = reinterpret_cast<UpdateFn>(replacement);
    } else if (target == reinterpret_cast<void *>(&dispatch_final)) {
        *original = reinterpret_cast<void *>(&fake_digest_final);
        g_hooked_final = reinterpret_cast<FinalFn>(replacement);
    }
    return 0;
}
int hook_uninstall(void *, void *target, char *, size_t) {
    ++g_hook_uninstalls;
    if (target == reinterpret_cast<void *>(&dispatch_init)) g_hooked_init = nullptr;
    if (target == reinterpret_cast<void *>(&dispatch_update)) g_hooked_update = nullptr;
    if (target == reinterpret_cast<void *>(&dispatch_final)) g_hooked_final = nullptr;
    return 0;
}
int rpc_register(void *, const DejavuRpcServiceDescriptor *desc, uint64_t *handle, char *, size_t) {
    g_rpc_desc = *desc;
    g_rpc_methods.assign(desc->methods, desc->methods + desc->method_count);
    g_rpc_route = desc->route_count > 0 ? desc->routes[0] : "";
    *handle = 202;
    return DEJAVU_RPC_OK;
}
int rpc_unregister(void *, uint64_t handle, char *, size_t) {
    assert(handle == 202);
    ++g_rpc_unregisters;
    return DEJAVU_RPC_OK;
}

const DejavuModuleInterface g_mod_iface = {DEJAVU_MODULE_INTERFACE_VERSION, sizeof(g_mod_iface),
                                           mod_resolve};
const DejavuHookInterface g_hook_iface = {DEJAVU_HOOK_INTERFACE_VERSION, sizeof(g_hook_iface),
                                          hook_install, hook_uninstall};
const DejavuRpcInterface g_rpc_iface = {DEJAVU_RPC_INTERFACE_VERSION, sizeof(g_rpc_iface),
                                        rpc_register, nullptr, rpc_unregister};

const void *host_get_interface(void *, const char *name, uint32_t version) {
    if (std::string(name) == "module" && version == DEJAVU_MODULE_INTERFACE_VERSION)
        return &g_mod_iface;
    if (std::string(name) == "hook" && version == DEJAVU_HOOK_INTERFACE_VERSION)
        return &g_hook_iface;
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

    g_rpc_available = true;
    state = nullptr;
    assert(plugin->initialize(&host, &state, err, sizeof(err)) == 0 && state != nullptr);
    assert(g_rpc_route.rfind("algorithm.", 0) == 0);

    int ok = 0;
    // Start in memory-only mode (no path or stream required).
    std::string body = call_rpc("start", "{}", &ok);
    assert(ok == 1 && body.find("\"active\":true") != std::string::npos &&
           body.find("\"hooks\":3") != std::string::npos);

    body = call_rpc("selftest", "{}", &ok);
    assert(ok == 1 && body.find("900150983cd24fb0d6963f7d28e17f72") != std::string::npos &&
           body.find("\"events\":3") != std::string::npos);

    body = call_rpc("recent", "{\"limit\":10,\"clear\":true}", &ok);
    assert(ok == 1 && body.find("DIGEST_INIT") != std::string::npos &&
           body.find("DIGEST_UPDATE") != std::string::npos &&
           body.find("DIGEST_FINAL") != std::string::npos);

    body = call_rpc("stop", "{}", &ok);
    assert(ok == 1 && body.find("\"active\":false") != std::string::npos);
    assert(g_hook_uninstalls == 3);

    plugin->on_unload_prepare(state);
    plugin->shutdown(state);
    assert(g_rpc_unregisters == 1 && g_alloc_count == 0);
    std::puts("PASS: algorithm Router RPC, memory ring buffer, selftest, and safe unload");
    return 0;
}
