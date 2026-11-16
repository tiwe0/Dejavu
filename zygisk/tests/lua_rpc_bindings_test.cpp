#include "lua_rpc_bindings.h"
#include <fstream>
#include <sstream>
#include "cJSON.h"
#include "dejavu_json.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <new>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

// Fail one allocation on the control thread; transport threads stay unaffected.
thread_local long allocation_countdown = -1;
thread_local bool allocation_failed = false;

// Keep GCC from diagnosing the malloc/free implementation as a mismatched
// allocation pair after inlining these test-only global replacements.
[[gnu::noinline]] void *operator new(std::size_t size) {
    if (allocation_countdown >= 0 && allocation_countdown-- == 0) {
        allocation_countdown = -1;
        allocation_failed = true;
        throw std::bad_alloc();
    }
    if (void *memory = std::malloc(size ? size : 1))
        return memory;
    throw std::bad_alloc();
}
[[gnu::noinline]] void *operator new[](std::size_t size) {
    return ::operator new(size);
}
[[gnu::noinline]] void operator delete(void *memory) noexcept {
    std::free(memory);
}
[[gnu::noinline]] void operator delete[](void *memory) noexcept {
    std::free(memory);
}
[[gnu::noinline]] void operator delete(void *memory, std::size_t) noexcept {
    std::free(memory);
}
[[gnu::noinline]] void operator delete[](void *memory, std::size_t) noexcept {
    std::free(memory);
}

#define CHECK(value)                                                                               \
    do {                                                                                           \
        if (!(value)) {                                                                            \
            std::cerr << "FAIL " << __LINE__ << ": " #value "\n";                                  \
            std::abort();                                                                          \
        }                                                                                          \
    } while (0)
using namespace std::chrono_literals;

uint64_t now_ms() {
    timespec value{};
    clock_gettime(CLOCK_MONOTONIC, &value);
    return value.tv_sec * 1000ULL + value.tv_nsec / 1000000;
}
template <class Test> void eventually(Test test, int timeout = 2500) {
    uint64_t until = now_ms() + timeout;
    while (!test() && now_ms() < until)
        std::this_thread::sleep_for(2ms);
    CHECK(test());
}
struct Json {
    cJSON *value;
    explicit Json(const std::string &text)
        : value(cJSON_ParseWithLength(text.data(), text.size())) {
        CHECK(value);
    }
    ~Json() {
        cJSON_Delete(value);
    }
    std::string string(const char *key) const {
        auto *item = cJSON_GetObjectItemCaseSensitive(value, key);
        CHECK(cJSON_IsString(item));
        return item->valuestring;
    }
    std::string error() const {
        auto *object = cJSON_GetObjectItemCaseSensitive(value, "error");
        auto *code = cJSON_GetObjectItemCaseSensitive(object, "code");
        CHECK(cJSON_IsString(code));
        return code->valuestring;
    }
};
struct Peer {
    int fd = -1;
    std::string buffered;
    Peer() = default;
    explicit Peer(int socket) : fd(socket) {
    }
    Peer(Peer &&other) noexcept : fd(other.fd), buffered(std::move(other.buffered)) {
        other.fd = -1;
    }
    ~Peer() {
        close_peer();
    }
    void close_peer() {
        if (fd >= 0) {
            shutdown(fd, SHUT_RDWR);
            close(fd);
            fd = -1;
        }
    }
    void send_raw(const std::string &text) {
        size_t offset = 0;
        while (offset < text.size()) {
#ifdef MSG_NOSIGNAL
            ssize_t count = send(fd, text.data() + offset, text.size() - offset, MSG_NOSIGNAL);
#else
            ssize_t count = send(fd, text.data() + offset, text.size() - offset, 0);
#endif
            CHECK(count > 0);
            offset += static_cast<size_t>(count);
        }
    }
    void send_line(const std::string &text) {
        send_raw(text + "\n");
    }
    std::string receive(int timeout = 3000) {
        uint64_t until = now_ms() + timeout;
        while (buffered.find('\n') == std::string::npos) {
            CHECK(now_ms() < until);
            pollfd descriptor{fd, POLLIN, 0};
            CHECK(poll(&descriptor, 1, static_cast<int>(until - now_ms())) > 0);
            char data[8192];
            ssize_t count = recv(fd, data, sizeof(data), 0);
            CHECK(count > 0);
            buffered.append(data, static_cast<size_t>(count));
        }
        size_t newline = buffered.find('\n');
        std::string result = buffered.substr(0, newline);
        buffered.erase(0, newline + 1);
        return result;
    }
    bool quiet(int timeout = 150) {
        if (!buffered.empty())
            return false;
        pollfd descriptor{fd, POLLIN, 0};
        return poll(&descriptor, 1, timeout) == 0;
    }
    bool eof(int timeout = 3000) {
        pollfd descriptor{fd, POLLIN, 0};
        if (poll(&descriptor, 1, timeout) <= 0)
            return false;
        char byte;
        return recv(fd, &byte, 1, MSG_PEEK) == 0;
    }
};
struct Daemon {
    std::string directory, path, endpoint;
    int listener = -1;
    Daemon() {
        char name[] = "/tmp/dvrpc-XXXXXX";
        char *created = mkdtemp(name);
        CHECK(created);
        directory = created;
        path = directory + "/p.sock";
        endpoint = "unix:" + path;
        listener = socket(AF_UNIX, SOCK_STREAM, 0);
        CHECK(listener >= 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        memcpy(address.sun_path, path.c_str(), path.size() + 1);
        CHECK(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
        CHECK(listen(listener, 16) == 0);
    }
    ~Daemon() {
        close(listener);
        unlink(path.c_str());
        rmdir(directory.c_str());
    }
    bool pending(int timeout = 150) {
        pollfd descriptor{listener, POLLIN, 0};
        return poll(&descriptor, 1, timeout) > 0;
    }
    Peer accept_peer(bool acknowledge = true, const std::string &route = "route") {
        CHECK(pending(3500));
        int fd = accept(listener, nullptr, nullptr);
        CHECK(fd >= 0);
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        Peer peer(fd);
        Json hello(peer.receive());
        CHECK(hello.string("type") == "hello");
        CHECK(hello.string("app") == "org.example.demo");
        auto *routes = cJSON_GetObjectItemCaseSensitive(hello.value, "routes");
        CHECK(cJSON_GetArraySize(routes) == 1);
        CHECK(std::string(cJSON_GetArrayItem(routes, 0)->valuestring) == route);
        if (acknowledge)
            peer.send_line("{\"type\":\"hello_ok\",\"version\":1}");
        return peer;
    }
};

struct Gate {
    std::atomic<bool> entered{false}, release{false};
    static int block(dejavu_control_session *, void *opaque, const dejavu_value *, size_t,
                     dejavu_value *, char *, size_t) {
        auto *gate = static_cast<Gate *>(opaque);
        gate->entered = true;
        while (!gate->release)
            std::this_thread::sleep_for(1ms);
        return DEJAVU_OK;
    }
};
std::atomic<int> retained_refs{0}, released_refs{0};
struct RuntimeFixture {
    DejavuRuntime runtime{};
    dejavu_engine *engine = dejavu_engine_create();
    dejavu_control_session *session = nullptr;
    RpcService rpc;
    std::unique_ptr<LuaRpcBindings> bindings;
    RuntimeFixture(const std::string &bootstrap) {
        runtime.control_session_register = dejavu_control_session_register;
        runtime.control_session_eval = dejavu_control_session_eval;
        runtime.control_session_retain_function = [](dejavu_control_session *session,
                                                     unsigned long long source,
                                                     unsigned long long *out, char *error,
                                                     size_t size) {
            int status = dejavu_control_session_retain_function(session, source, out, error, size);
            if (status == DEJAVU_OK)
                ++retained_refs;
            return status;
        };
        runtime.control_session_invoke_cancellable = dejavu_control_session_invoke_cancellable;
        runtime.control_session_release_function = [](dejavu_control_session *session,
                                                      unsigned long long ref) {
            ++released_refs;
            dejavu_control_session_release_function(session, ref);
        };
        runtime.value_release = dejavu_value_release;
        char error[512]{};
        CHECK(dejavu_control_session_create(engine, &session, error, sizeof(error)) == 0);
        bindings = std::make_unique<LuaRpcBindings>(&runtime, session, &rpc);
        std::string detail;
        CHECK(bindings->initialize(bootstrap, &detail));
    }
    ~RuntimeFixture() {
        bindings.reset();
        dejavu_control_session_destroy(session);
        dejavu_engine_destroy(engine);
    }
    bool eval(const std::string &script) {
        dejavu_value result{};
        char error[512]{};
        int status =
            dejavu_control_session_eval(session, script.c_str(), &result, error, sizeof(error));
        if (status)
            std::cerr << error << "\n";
        if (result.type == DEJAVU_VALUE_FUNCTION)
            dejavu_control_session_release_function(session, result.function_value);
        dejavu_value_release(&result);
        return status == 0;
    }
};
std::string request(const std::string &id, const std::string &method = "echo",
                    const std::string &route = "route", int timeout = 1500) {
    return "{\"type\":\"request\",\"id\":\"" + id + "\",\"route\":\"" + route + "\",\"method\":\"" +
           method + "\",\"deadline_monotonic_ms\":" + std::to_string(now_ms() + timeout) +
           ",\"params\":{\"nested\":[null,true,{\"x\":3}]}}";
}
int json_allocations_before_failure = 0;
void test_exact_json_printing() {
    Json tree("{\"positive\":9007199254740991,\"negative\":-9007199254740991,"
              "\"nested\":[1.25,9007199254740991]}");
    auto *positive = cJSON_GetObjectItemCaseSensitive(tree.value, "positive");
    auto *negative = cJSON_GetObjectItemCaseSensitive(tree.value, "negative");
    const int original_type = positive->type | cJSON_IsReference;
    char marker[] = "original number storage";
    positive->type = original_type;
    positive->valuestring = marker;
    char *encoded = dejavu_json_print(tree.value);
    CHECK(encoded);
    const std::string printed(encoded);
    std::free(encoded);
    CHECK(printed.find("9007199254740991") != std::string::npos);
    CHECK(printed.find("-9007199254740991") != std::string::npos);
    CHECK(positive->type == original_type && positive->valuestring == marker);
    CHECK(cJSON_IsNumber(negative) && negative->valuestring == nullptr);
    CHECK(positive->valuedouble == 9007199254740991.0);
    cJSON_Hooks hooks{};
    hooks.malloc_fn = [](size_t size) -> void * {
        if (json_allocations_before_failure-- == 0)
            return nullptr;
        return std::malloc(size);
    };
    hooks.free_fn = std::free;
    // Fail while building restoration records, then during the actual print.
    for (int allocations : {1, 3}) {
        json_allocations_before_failure = allocations;
        cJSON_InitHooks(&hooks);
        encoded = dejavu_json_print(tree.value);
        cJSON_InitHooks(nullptr);
        CHECK(!encoded);
        CHECK(positive->type == original_type && positive->valuestring == marker);
        CHECK(cJSON_IsNumber(negative) && negative->valuestring == nullptr);
    }
    encoded = dejavu_json_print(tree.value);
    CHECK(encoded && printed == encoded);
    std::free(encoded);
    std::cout
        << "PASS: safe integer printing preserves tree state on success and allocation failure\n";
}
int arm_allocation(dejavu_control_session *, void *, const dejavu_value *args, size_t count,
                   dejavu_value *, char *, size_t) {
    CHECK(count == 1 && args[0].type == DEJAVU_VALUE_INTEGER);
    allocation_failed = false;
    allocation_countdown = args[0].integer_value;
    return DEJAVU_OK;
}
int disarm_allocation(dejavu_control_session *, void *, const dejavu_value *, size_t,
                      dejavu_value *, char *, size_t) {
    allocation_countdown = -1;
    return DEJAVU_OK;
}
void test_allocation_failures(const std::string &bootstrap, bool active) {
    Daemon daemon;
    RuntimeFixture f(bootstrap);
    char error[256]{};
    CHECK(dejavu_control_session_register(f.session, "test", "arm", arm_allocation, nullptr, error,
                                          sizeof(error)) == DEJAVU_OK);
    CHECK(dejavu_control_session_register(f.session, "test", "disarm", disarm_allocation, nullptr,
                                          error, sizeof(error)) == DEJAVU_OK);
    CHECK(f.eval("endpoint='" + daemon.endpoint + "'"));
    if (active)
        f.bindings->activate();
    int failures = 0;
    for (int site = 0; site < 64; ++site) {
        const auto script =
            "local spec={name='oom',app='org.example.demo',routes={},"
            "methods={echo=function(p) return p end},endpoint=endpoint}; test.arm(" +
            std::to_string(site) +
            "); local ok,service=pcall(rpc.register,spec); "
            "test.disarm(); if ok then assert(service:unregister()) "
            "else assert(type(service)=='string') end";
        CHECK(f.eval(script));
        failures += allocation_failed ? 1 : 0;
        CHECK(retained_refs == released_refs);
        // Same-name recovery proves failure did not publish an orphan service.
        CHECK(f.eval("recovered=rpc.register{name='oom',app='org.example.demo',routes={},"
                     "methods={echo=function(p) return p end},endpoint=endpoint}"));
        if (active) {
            CHECK(f.eval("recovered:update_routes({'route'})"));
            auto peer = daemon.accept_peer();
            peer.send_line(request("recovered"));
            CHECK(Json(peer.receive()).string("id") == "recovered");
            CHECK(f.eval("assert(recovered:unregister()); recovered=nil"));
            CHECK(peer.eof());
        } else
            CHECK(f.eval("assert(recovered:unregister()); recovered=nil"));
        CHECK(retained_refs == released_refs);
        CHECK(!daemon.pending(1));
    }
    CHECK(failures > 8);
    std::cout << "PASS: " << (active ? "active" : "inactive")
              << " Lua registration allocation sweep (" << failures << " sites)\n";
}
int native_echo(void *, const DejavuRpcRequest *request, DejavuRpcResponse *response, char *,
                size_t) {
    response->ok = 1;
    response->payload_json = static_cast<char *>(std::malloc(request->params_size + 1));
    CHECK(response->payload_json);
    std::memcpy(response->payload_json, request->params_json, request->params_size);
    response->payload_json[request->params_size] = 0;
    response->payload_size = request->params_size;
    return DEJAVU_RPC_OK;
}
void test_shared_budget_and_lifetime(const std::string &bootstrap) {
    Daemon native_daemon, lua_daemon;
    RuntimeFixture f(bootstrap);
    const char *routes[] = {"route"};
    DejavuRpcMethod method{"echo", native_echo};
    DejavuRpcServiceDescriptor descriptor{sizeof(descriptor),
                                          "native",
                                          "org.example.demo",
                                          routes,
                                          1,
                                          &method,
                                          1,
                                          nullptr,
                                          native_daemon.endpoint.c_str()};
    uint64_t native_handle = 0;
    std::string error;
    CHECK(f.rpc.register_service(42, &descriptor, &native_handle, &error) == DEJAVU_RPC_OK);
    f.rpc.activate_owner(42);
    auto native_peer = native_daemon.accept_peer();
    CHECK(f.eval("endpoint='" + lua_daemon.endpoint +
                 "'; services={}; "
                 "for i=1,15 do services[i]=rpc.register{name='lua'..i,app='org.example.demo',"
                 "routes=i==1 and {'route'} or {},methods={echo=function(p) return p "
                 "end},endpoint=endpoint} end; "
                 "assert(not pcall(rpc.register,{name='overflow',app='org.example.demo',routes={},"
                 "methods={echo=function(p) return p end},endpoint=endpoint}))"));
    f.bindings->activate();
    auto lua_peer = lua_daemon.accept_peer();
    lua_peer.send_line(request("lua-before"));
    CHECK(Json(lua_peer.receive()).string("id") == "lua-before");
    native_peer.send_line(request("native-before"));
    CHECK(Json(native_peer.receive()).string("id") == "native-before");
    f.bindings->shutdown();
    CHECK(lua_peer.eof());
    CHECK(retained_refs == released_refs);
    native_peer.send_line(request("native-after"));
    CHECK(Json(native_peer.receive()).string("id") == "native-after");
    CHECK(f.rpc.unregister_service(42, native_handle, &error) == DEJAVU_RPC_OK);
    CHECK(native_peer.eof());
    std::cout
        << "PASS: native and Lua share the 16-service budget; Lua shutdown preserves native RPC\n";
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    std::ifstream file(argv[1]);
    std::stringstream source;
    source << file.rdbuf();
    test_exact_json_printing();
    test_allocation_failures(source.str(), false);
    test_allocation_failures(source.str(), true);
    test_shared_budget_and_lifetime(source.str());
    Daemon daemon;
    RuntimeFixture f(source.str());
    std::string setup = "endpoint = '" + daemon.endpoint + "'; ";
    CHECK(f.eval(setup + R"LUA(
        local methods = {
            echo = function(params, request)
                saved_cancel = request.is_cancelled
                assert(not request.is_cancelled())
                assert(request.id and request.route == 'route' and request.method == 'echo')
                assert(request.deadline_monotonic_ms > 0)
                return params
            end,
            blocked = function() blocked_calls = (blocked_calls or 0) + 1; return true end,
            fail = function() rpc.error('demo.invalid', 'Synthetic failure', true) end,
            crash = function() error('secret body') end,
            self = function() local ok, why = service:unregister(); assert(not ok and why == 'busy'); return true end,
        }
        service = rpc.register{name='lua',app='org.example.demo',routes={'route'},methods=methods,endpoint=endpoint}
        methods.echo = function() error('mutated method') end
        methods = nil; collectgarbage('collect')
        assert(not pcall(rpc.register, {name='bad',app='org.example.demo',routes={'route'},methods={},endpoint=endpoint}))
        assert(not pcall(rpc.register, {name='bad',app='org.example.demo',routes={'route'},methods={echo=function() end},endpoint='invalid'}))
        assert(not pcall(rpc.register, {name='bad',app='org.example.demo',routes={[2]='gap'},methods={echo=function() end},endpoint=endpoint}))
        for _, routes in ipairs({'["route"] trailing', '["bad\\u0000route"]', '["route"]' .. string.char(0)}) do
            assert(not pcall(rpc._register, 'bad', 'org.example.demo', routes,
                             '["echo"]', function() return '{}' end, endpoint))
        end
    )LUA"));
    CHECK(retained_refs == released_refs + 1);
    CHECK(!daemon.pending());
    f.bindings->activate();
    auto peer = daemon.accept_peer();
    peer.send_line(request("nested"));
    Json nested(peer.receive());
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(nested.value, "ok")));
    auto *result = cJSON_GetObjectItemCaseSensitive(nested.value, "result");
    CHECK(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(result, "nested")));
    CHECK(f.eval("assert(saved_cancel())"));
    for (const std::string &params :
         {std::string("9007199254740991"), std::string("-9007199254740991"),
          std::string("{\"nested\":[9007199254740991,-9007199254740991]}")}) {
        peer.send_line("{\"type\":\"request\",\"id\":\"exact\",\"route\":\"route\","
                       "\"method\":\"echo\",\"params\":" +
                       params + "}");
        const auto wire = peer.receive();
        CHECK(wire.find(params) != std::string::npos);
        Json exact(wire);
        auto *value = cJSON_GetObjectItemCaseSensitive(exact.value, "result");
        if (params.front() == '{')
            value = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(value, "nested"), 0);
        CHECK(cJSON_IsNumber(value));
        CHECK(value->valuedouble ==
              (params.front() == '-' ? -9007199254740991.0 : 9007199254740991.0));
    }
    peer.send_line(request("business", "fail"));
    CHECK(Json(peer.receive()).error() == "demo.invalid");
    peer.send_line(request("crash", "crash"));
    CHECK(Json(peer.receive()).error() == "rpc.handler_failed");
    Gate gate;
    char detail[256]{};
    CHECK(dejavu_control_session_register(f.session, "test", "block", Gate::block, &gate, detail,
                                          sizeof(detail)) == DEJAVU_OK);
    std::thread control([&] { CHECK(f.eval("test.block()")); });
    eventually([&] { return gate.entered.load(); });
    peer.send_line(request("expired-lock", "blocked", "route", 50));
    peer.send_line(request("cancelled-lock", "blocked"));
    peer.send_line("{\"type\":\"cancel\",\"id\":\"cancelled-lock\",\"route\":\"route\"}");
    CHECK(peer.quiet(180));
    gate.release = true;
    control.join();
    peer.send_line(request("barrier"));
    CHECK(Json(peer.receive()).string("id") == "barrier");
    CHECK(f.eval("assert(blocked_calls == nil)"));
    CHECK(f.eval("service:update_routes({})"));
    CHECK(peer.eof());
    CHECK(!daemon.pending());
    CHECK(f.eval("service:update_routes({'next'})"));
    auto replacement = daemon.accept_peer(true, "next");
    replacement.send_line(request("self", "self", "next"));
    CHECK(replacement.eof());
    CHECK(f.eval("assert(service:unregister()); assert(service:unregister())"));
    CHECK(f.eval("second = "
                 "rpc.register{name='second',app='org.example.demo',routes={'route'},methods={echo="
                 "function(p) return p end},endpoint=endpoint}"));
    auto second = daemon.accept_peer();
    second.send_line(request("second"));
    CHECK(Json(second.receive()).string("id") == "second");
    CHECK(f.eval("third = "
                 "rpc.register{name='third',app='org.example.demo',routes={'route'},methods={echo="
                 "function(p) return p end},endpoint=endpoint}"));
    auto third = daemon.accept_peer();
    second.send_line(request("parallel-second"));
    third.send_line(request("parallel-third"));
    CHECK(f.eval("assert(2 + 2 == 4)"));
    CHECK(Json(second.receive()).string("id") == "parallel-second");
    CHECK(Json(third.receive()).string("id") == "parallel-third");
    f.bindings->shutdown();
    CHECK(third.eof());
    CHECK(retained_refs == released_refs);
    CHECK(second.eof());
    CHECK(f.eval("assert(saved_cancel())"));
    std::cout << "PASS: Lua RPC GC retention, method snapshot, nested JSON, errors, routes, BUSY "
                 "retry and teardown\n";
}
