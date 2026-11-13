#include "lua_rpc_bindings.h"
#include "cJSON.h"
#include "dejavu_json.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {
struct Invocation {
    LuaRpcBindings *adapter;
    uint64_t token;
    const DejavuRpcRequest *request;
};
thread_local const Invocation *current_invocation = nullptr;
std::atomic<uint64_t> next_token{1};
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
int fail(char *error, size_t size, const char *message) {
    if (error && size)
        std::snprintf(error, size, "%s", message);
    return DEJAVU_ERROR_INVALID_ARGUMENT;
}
bool text(const dejavu_value &value) {
    return value.type == DEJAVU_VALUE_STRING && value.string_value &&
           !std::memchr(value.string_value, 0, value.string_size);
}
std::string string(const dejavu_value &value) {
    return std::string(value.string_value, value.string_size);
}
bool strings(const dejavu_value &value, std::vector<std::string> *out) {
    if (!text(value) || value.string_size > 1024 * 1024)
        return false;
    const auto source = string(value);
    // cJSON represents strings as C strings: reject escaped NUL before decoding.
    size_t slashes = 0;
    for (size_t i = 0; i < source.size(); ++i) {
        if (source[i] == '\\') {
            ++slashes;
            continue;
        }
        if ((slashes % 2) && source.compare(i, 5, "u0000") == 0)
            return false;
        slashes = 0;
    }
    Json json(cJSON_ParseWithLengthOpts(source.c_str(), source.size() + 1, nullptr, 1),
              cJSON_Delete);
    if (!cJSON_IsArray(json.get()) || cJSON_GetArraySize(json.get()) > 64)
        return false;
    for (auto *item = json->child; item; item = item->next) {
        if (!cJSON_IsString(item) || !item->valuestring[0])
            return false;
        out->emplace_back(item->valuestring);
    }
    return true;
}
std::vector<const char *> pointers(const std::vector<std::string> &values) {
    std::vector<const char *> result;
    for (const auto &value : values)
        result.push_back(value.c_str());
    return result;
}
dejavu_value str(const char *value, size_t size) {
    dejavu_value result{};
    result.type = DEJAVU_VALUE_STRING;
    result.string_value = value;
    result.string_size = size;
    return result;
}
dejavu_value str(const char *value) {
    return str(value, std::strlen(value));
}
dejavu_value integer(uint64_t value) {
    dejavu_value result{};
    result.type = DEJAVU_VALUE_INTEGER;
    result.integer_value = static_cast<long long>(value);
    return result;
}
int is_cancelled(void *opaque) {
    auto *request = static_cast<const DejavuRpcRequest *>(opaque);
    return request->is_cancelled && request->is_cancelled(request->cancellation_context);
}
} // namespace

struct LuaRpcBindings::Service {
    LuaRpcBindings *adapter;
    unsigned long long function = 0;
    explicit Service(LuaRpcBindings *owner) : adapter(owner) {
    }
    ~Service() {
        if (function)
            adapter->runtime_->control_session_release_function(adapter->session_, function);
    }
};
LuaRpcBindings::LuaRpcBindings(DejavuRuntime *runtime, dejavu_control_session *session,
                               RpcService *rpc)
    : runtime_(runtime), session_(session), rpc_(rpc) {
}
LuaRpcBindings::~LuaRpcBindings() {
    shutdown();
}
bool LuaRpcBindings::initialize(const std::string &bootstrap, std::string *error) {
    if (initialized_ || stopped_ || !runtime_ || !session_ || !rpc_) {
        if (error)
            *error = "Invalid Lua RPC adapter state";
        return false;
    }
    struct Binding {
        const char *name;
        dejavu_host_function callback;
    };
    const Binding bindings[] = {{"_register", host_register},
                                {"_update_routes", host_update},
                                {"_unregister", host_unregister},
                                {"_is_cancelled", host_cancelled}};
    char detail[256]{};
    for (const auto &binding : bindings) {
        if (runtime_->control_session_register(session_, "rpc", binding.name, binding.callback,
                                               this, detail, sizeof(detail)) != DEJAVU_OK) {
            if (error)
                *error = detail;
            return false;
        }
    }
    dejavu_value result{};
    int status = runtime_->control_session_eval(session_, bootstrap.c_str(), &result, detail,
                                                sizeof(detail));
    if (result.type == DEJAVU_VALUE_FUNCTION)
        runtime_->control_session_release_function(session_, result.function_value);
    runtime_->value_release(&result);
    if (status != DEJAVU_OK) {
        if (error)
            *error = detail;
        return false;
    }
    initialized_ = true;
    return true;
}
void LuaRpcBindings::activate() {
    if (initialized_ && !stopped_)
        rpc_->activate_owner(kLuaRpcOwnerToken);
}
void LuaRpcBindings::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_.exchange(true))
            return;
    }
    if (!rpc_)
        return;
    while (!rpc_->retire_owner(kLuaRpcOwnerToken))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::map<uint64_t, std::shared_ptr<Service>> released;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        released.swap(services_);
    }
    // Destruction may take the Lua session lock; never hold the adapter lock here.
}
int LuaRpcBindings::host_register(dejavu_control_session *, void *opaque, const dejavu_value *args,
                                  size_t count, dejavu_value *result, char *error,
                                  size_t size) try {
    auto *self = static_cast<LuaRpcBindings *>(opaque);
    if (self->stopped_ || count != 6 || !text(args[0]) || !text(args[1]) ||
        args[4].type != DEJAVU_VALUE_FUNCTION || !text(args[5]))
        return fail(error, size, "Invalid RPC registration");
    std::vector<std::string> routes, names;
    if (!strings(args[2], &routes) || !strings(args[3], &names))
        return fail(error, size, "Invalid RPC routes or methods");
    auto service = std::make_shared<Service>(self);
    // Reserve the owning registry node before publishing its raw callback
    // context to the transport. Node insertion below cannot allocate.
    std::map<uint64_t, std::shared_ptr<Service>> pending;
    pending.emplace(0, service);
    auto node = pending.extract(pending.begin());
    int status = self->runtime_->control_session_retain_function(
        self->session_, args[4].function_value, &service->function, error, size);
    if (status != DEJAVU_OK)
        return status;
    auto route_ptrs = pointers(routes);
    std::vector<DejavuRpcMethod> methods;
    for (const auto &name : names)
        methods.push_back({name.c_str(), dispatch});
    auto name = string(args[0]), app = string(args[1]), endpoint = string(args[5]);
    DejavuRpcServiceDescriptor descriptor{
        sizeof(descriptor), name.c_str(),      app.c_str(),
        route_ptrs.data(),  route_ptrs.size(), methods.data(),
        methods.size(),     service.get(),     endpoint.empty() ? nullptr : endpoint.c_str()};
    uint64_t handle = 0;
    std::string detail;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        if (self->stopped_)
            detail = "Lua RPC adapter is stopped";
        else if (self->rpc_->register_service(kLuaRpcOwnerToken, &descriptor, &handle, &detail) ==
                 DEJAVU_RPC_OK) {
            node.key() = handle;
            self->services_.insert(std::move(node));
        }
    }
    if (!handle)
        return fail(error, size, detail.c_str());
    *result = integer(handle);
    return DEJAVU_OK;
} catch (...) {
    return fail(error, size, "Unable to register Lua RPC service");
}
int LuaRpcBindings::host_update(dejavu_control_session *, void *opaque, const dejavu_value *args,
                                size_t count, dejavu_value *, char *error, size_t size) try {
    auto *self = static_cast<LuaRpcBindings *>(opaque);
    if (count != 2 || args[0].type != DEJAVU_VALUE_INTEGER)
        return fail(error, size, "Invalid RPC route update");
    std::vector<std::string> routes;
    if (!strings(args[1], &routes))
        return fail(error, size, "Invalid RPC routes");
    auto ptrs = pointers(routes);
    std::string detail;
    if (self->rpc_->update_routes(kLuaRpcOwnerToken, args[0].integer_value, ptrs.data(),
                                  ptrs.size(), &detail) != DEJAVU_RPC_OK)
        return fail(error, size, detail.c_str());
    return DEJAVU_OK;
} catch (...) {
    return fail(error, size, "Unable to update Lua RPC routes");
}
int LuaRpcBindings::host_unregister(dejavu_control_session *, void *opaque,
                                    const dejavu_value *args, size_t count, dejavu_value *result,
                                    char *error, size_t size) try {
    auto *self = static_cast<LuaRpcBindings *>(opaque);
    if (count != 1 || args[0].type != DEJAVU_VALUE_INTEGER)
        return fail(error, size, "Invalid RPC unregister");
    std::string detail;
    int status = self->rpc_->unregister_service(kLuaRpcOwnerToken, args[0].integer_value, &detail);
    if (status == DEJAVU_RPC_ERROR)
        return fail(error, size, detail.c_str());
    std::shared_ptr<Service> released;
    if (status == DEJAVU_RPC_OK) {
        std::lock_guard<std::mutex> lock(self->mutex_);
        auto found = self->services_.find(args[0].integer_value);
        if (found != self->services_.end()) {
            released = std::move(found->second);
            self->services_.erase(found);
        }
    }
    *result = integer(static_cast<uint64_t>(status));
    return DEJAVU_OK;
} catch (...) {
    return fail(error, size, "Unable to unregister Lua RPC service");
}
int LuaRpcBindings::host_cancelled(dejavu_control_session *, void *opaque, const dejavu_value *args,
                                   size_t count, dejavu_value *result, char *error,
                                   size_t size) try {
    if (count != 1 || args[0].type != DEJAVU_VALUE_INTEGER)
        return fail(error, size, "Invalid RPC cancellation token");
    result->type = DEJAVU_VALUE_BOOLEAN;
    result->boolean_value =
        !current_invocation || current_invocation->adapter != opaque ||
        current_invocation->token != static_cast<uint64_t>(args[0].integer_value) ||
        is_cancelled(const_cast<DejavuRpcRequest *>(current_invocation->request));
    return DEJAVU_OK;
} catch (...) {
    return fail(error, size, "Unable to inspect Lua RPC cancellation");
}
int LuaRpcBindings::dispatch(void *opaque, const DejavuRpcRequest *request,
                             DejavuRpcResponse *response, char *, size_t) {
    auto *service = static_cast<Service *>(opaque);
    auto *self = service->adapter;
    Invocation invocation{self, next_token.fetch_add(1), request};
    const auto *previous = current_invocation;
    current_invocation = &invocation;
    struct Restore {
        const Invocation *previous;
        ~Restore() {
            current_invocation = previous;
        }
    } restore{previous};
    dejavu_value args[] = {str(request->params_json, request->params_size),
                           str(request->id),
                           str(request->route),
                           str(request->method),
                           integer(request->deadline_monotonic_ms),
                           integer(invocation.token)};
    dejavu_value result{};
    char detail[256]{};
    int status = self->runtime_->control_session_invoke_cancellable(
        self->session_, service->function, args, 6, &result, is_cancelled,
        const_cast<DejavuRpcRequest *>(request), detail, sizeof(detail));
    Json envelope(nullptr, cJSON_Delete);
    if (status == DEJAVU_OK && result.type == DEJAVU_VALUE_STRING)
        envelope.reset(cJSON_ParseWithLength(result.string_value, result.string_size));
    auto *ok = cJSON_GetObjectItemCaseSensitive(envelope.get(), "ok");
    auto *payload =
        cJSON_GetObjectItemCaseSensitive(envelope.get(), cJSON_IsTrue(ok) ? "result" : "error");
    char *encoded = cJSON_IsBool(ok) && payload ? dejavu_json_print(payload) : nullptr;
    if (result.type == DEJAVU_VALUE_FUNCTION)
        self->runtime_->control_session_release_function(self->session_, result.function_value);
    self->runtime_->value_release(&result);
    if (!encoded)
        return DEJAVU_RPC_ERROR;
    response->ok = cJSON_IsTrue(ok);
    response->payload_json = encoded;
    response->payload_size = std::strlen(encoded);
    return DEJAVU_RPC_OK;
}
