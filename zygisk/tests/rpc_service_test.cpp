#include "rpc_service.h"
#include "cJSON.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

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
struct State {
    std::atomic<int> calls{0};
    std::atomic<bool> entered{false}, release{false}, cancelled{false};
};
void respond(DejavuRpcResponse *response, const std::string &text, bool ok = true) {
    response->ok = ok;
    response->payload_json = static_cast<char *>(malloc(text.size() + 1));
    CHECK(response->payload_json);
    memcpy(response->payload_json, text.c_str(), text.size() + 1);
    response->payload_size = text.size();
}
int handle(void *opaque, const DejavuRpcRequest *request, DejavuRpcResponse *response, char *,
           size_t) {
    auto &state = *static_cast<State *>(opaque);
    ++state.calls;
    CHECK(request->struct_size == sizeof(*request));
    const std::string method(request->method);
    if (method == "wait" || method == "ignore") {
        state.entered.store(true);
        while (!state.release.load()) {
            if (method == "wait" && request->is_cancelled(request->cancellation_context)) {
                state.cancelled.store(true);
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
    }
    if (method == "huge")
        respond(response, "\"" + std::string(1024 * 1024 - 2, 'x') + "\"");
    else if (method == "bad")
        respond(response, "not-json");
    else if (method == "fail")
        respond(
            response,
            "{\"code\":\"business.invalid\",\"message\":\"Synthetic error\",\"retryable\":false}",
            false);
    else if (method == "reserved")
        respond(response, "{\"code\":\"rpc.forged\",\"message\":\"Synthetic error\"}", false);
    else if (method == "throw") {
        respond(response, "{}");
        throw std::runtime_error("secret must not escape");
    } else
        respond(response, std::string(request->params_json, request->params_size));
    return DEJAVU_RPC_OK;
}
const DejavuRpcMethod methods[] = {{"echo", handle},  {"wait", handle}, {"ignore", handle},
                                   {"bad", handle},   {"fail", handle}, {"reserved", handle},
                                   {"throw", handle}, {"huge", handle}};
uint64_t register_service(RpcService &service, Daemon &daemon, State &state, uint64_t owner = 1,
                          bool zero_routes = false) {
    const char *route = "route";
    DejavuRpcServiceDescriptor descriptor{sizeof(descriptor),
                                          "demo",
                                          "org.example.demo",
                                          zero_routes ? nullptr : &route,
                                          zero_routes ? 0u : 1u,
                                          methods,
                                          sizeof(methods) / sizeof(methods[0]),
                                          &state,
                                          daemon.endpoint.c_str()};
    uint64_t handle = 0;
    std::string error;
    CHECK(service.register_service(owner, &descriptor, &handle, &error) == DEJAVU_RPC_OK);
    CHECK(handle);
    return handle;
}
std::string request(const std::string &id, const std::string &method = "echo",
                    const std::string &route = "route", int budget = 3000) {
    return "{\"type\":\"request\",\"id\":\"" + id + "\",\"route\":\"" + route + "\",\"method\":\"" +
           method + "\",\"params\":{\"value\":42},\"deadline_monotonic_ms\":" +
           std::to_string(now_ms() + budget) + "}";
}
void test_lifecycle_validation_and_methods() {
    Daemon daemon;
    State state;
    RpcService service;
    uint64_t token = register_service(service, daemon, state);
    CHECK(!daemon.pending());
    service.activate_owner(1);
    Peer peer = daemon.accept_peer();
    peer.send_line(request("one"));
    Json first(peer.receive());
    CHECK(first.string("id") == "one");
    auto *result = cJSON_GetObjectItemCaseSensitive(first.value, "result");
    CHECK(cJSON_GetObjectItemCaseSensitive(result, "value")->valueint == 42);
    const std::vector<std::pair<std::string, std::string>> failures = {
        {"absent", "rpc.method_not_found"}, {"huge", "rpc.invalid_response"},
        {"bad", "rpc.invalid_response"},    {"reserved", "rpc.invalid_response"},
        {"throw", "rpc.handler_failed"},    {"fail", "business.invalid"}};
    for (const auto &failure : failures) {
        peer.send_line(request(failure.first, failure.first));
        Json response(peer.receive());
        CHECK(response.error() == failure.second);
    }
    peer.send_line(request("wrong-route", "echo", "other"));
    Json mismatch(peer.receive());
    CHECK(mismatch.error() == "rpc.route_mismatch");
    std::string error;
    CHECK(service.unregister_service(2, token, &error) == DEJAVU_RPC_ERROR);
    const char *new_route = "new";
    CHECK(service.update_routes(2, token, &new_route, 1, &error) == DEJAVU_RPC_ERROR);
    CHECK(service.unregister_service(1, token, &error) == DEJAVU_RPC_OK);
    CHECK(peer.eof());
    CHECK(service.retire_owner(1));
    DejavuRpcServiceDescriptor invalid{sizeof(invalid), "bad", "app",  nullptr,          0,
                                       methods,         1,     &state, "127.0.0.1:39111"};
    CHECK(service.register_service(3, &invalid, &token, &error) == DEJAVU_RPC_ERROR);
    invalid.endpoint = daemon.endpoint.c_str();
    CHECK(service.register_service(1, &invalid, &token, &error) == DEJAVU_RPC_ERROR);
    std::cout << "PASS: activation, result/error contract, method whitelist, owner isolation, "
                 "Unix-only endpoints\n";
}
void test_zero_routes_and_update() {
    Daemon daemon;
    State state;
    RpcService service;
    uint64_t token = register_service(service, daemon, state, 1, true);
    service.activate_owner(1);
    CHECK(!daemon.pending());
    const char *route = "route";
    std::string error;
    CHECK(service.update_routes(1, token, &route, 1, &error) == DEJAVU_RPC_OK);
    Peer peer = daemon.accept_peer();
    peer.send_line(request("blocked", "ignore"));
    eventually([&] { return state.entered.load(); });
    CHECK(service.update_routes(1, token, &route, 1, &error) == DEJAVU_RPC_OK);
    CHECK(peer.quiet(75));
    const char *next = "next";
    CHECK(service.update_routes(1, token, &next, 1, &error) == DEJAVU_RPC_OK);
    CHECK(peer.eof());
    Peer replacement = daemon.accept_peer(true, "next");
    replacement.send_line(request("new", "echo", "next"));
    state.release.store(true);
    Json response(replacement.receive());
    CHECK(response.string("id") == "new");
    CHECK(replacement.quiet());
    CHECK(service.update_routes(1, token, nullptr, 0, &error) == DEJAVU_RPC_OK);
    CHECK(replacement.eof());
    CHECK(!daemon.pending());
    std::cout << "PASS: empty-route idle state, route updates reconnect, old-generation responses "
                 "discarded\n";
}
void test_deadline_and_cancel() {
    Daemon daemon;
    State state;
    RpcService service;
    register_service(service, daemon, state);
    service.activate_owner(1);
    Peer peer = daemon.accept_peer();
    peer.send_line(request("deadline", "wait", "route", 80));
    eventually([&] { return state.entered.load(); });
    eventually([&] { return state.cancelled.load(); });
    CHECK(peer.quiet());
    state.entered.store(false);
    state.cancelled.store(false);
    peer.send_line(request("cancel", "wait"));
    eventually([&] { return state.entered.load(); });
    peer.send_line("{\"type\":\"cancel\",\"id\":\"cancel\",\"route\":\"route\"}");
    eventually([&] { return state.cancelled.load(); });
    CHECK(peer.quiet());
    peer.send_line(request("after"));
    Json response(peer.receive());
    CHECK(response.string("id") == "after");
    std::cout << "PASS: propagated monotonic deadline and cooperative cancellation without late "
                 "response\n";
}
void test_disconnect_and_handshake() {
    Daemon daemon;
    State state;
    RpcService service;
    register_service(service, daemon, state);
    service.activate_owner(1);
    Peer unacknowledged = daemon.accept_peer(false);
    unacknowledged.send_line(request("premature"));
    CHECK(unacknowledged.eof());
    CHECK(state.calls.load() == 0);
    Peer peer = daemon.accept_peer();
    peer.send_line(request("old", "wait"));
    eventually([&] { return state.entered.load(); });
    peer.close_peer();
    eventually([&] { return state.cancelled.load(); });
    Peer replacement = daemon.accept_peer();
    CHECK(replacement.quiet());
    replacement.send_line(request("new"));
    Json result(replacement.receive());
    CHECK(result.string("id") == "new");
    CHECK(state.calls.load() == 2);
    std::cout
        << "PASS: handshake gates dispatch; reconnect cancels old requests and never replays\n";
}
void test_busy_retirement_and_queue() {
    Daemon daemon;
    State state;
    RpcService service;
    uint64_t token = register_service(service, daemon, state);
    service.activate_owner(1);
    Peer peer = daemon.accept_peer();
    peer.send_line(request("blocked", "ignore"));
    eventually([&] { return state.entered.load(); });
    for (int i = 0; i < 17; ++i)
        peer.send_line(request("queued-" + std::to_string(i)));
    Json full(peer.receive());
    CHECK(full.error() == "rpc.busy");
    CHECK(full.string("id") == "queued-16");
    peer.send_line("{\"type\":\"cancel\",\"id\":\"queued-0\",\"route\":\"route\"}");
    std::string error;
    uint64_t started = now_ms();
    CHECK(service.unregister_service(1, token, &error) == DEJAVU_RPC_BUSY);
    CHECK(now_ms() - started < 1000);
    CHECK(!service.retire_owner(1));
    CHECK(state.calls.load() == 1);
    state.release.store(true);
    eventually([&] { return service.retire_owner(1); });
    CHECK(state.calls.load() == 1);
    std::cout << "PASS: bounded queue, cancel queued work, busy unload retains active callback "
                 "until drained\n";
}
void test_final_teardown_waits_for_callback() {
    Daemon daemon;
    State state;
    RpcService service;
    uint64_t token = register_service(service, daemon, state);
    service.activate_owner(1);
    Peer peer = daemon.accept_peer();
    peer.send_line(request("blocked", "ignore"));
    eventually([&] { return state.entered.load(); });
    std::atomic<bool> finished{false};
    std::thread teardown([&] {
        service.stop_and_join();
        finished.store(true);
    });
    std::this_thread::sleep_for(100ms);
    CHECK(!finished.load());
    state.release.store(true);
    teardown.join();
    CHECK(finished.load());
    service.stop_and_join();
    std::string error;
    CHECK(service.unregister_service(1, token, &error) == DEJAVU_RPC_ERROR);
    const char *route = "route";
    DejavuRpcServiceDescriptor descriptor{
        sizeof(descriptor),     "new", "org.example.demo", &route, 1, methods, 1, &state,
        daemon.endpoint.c_str()};
    CHECK(service.register_service(2, &descriptor, &token, &error) == DEJAVU_RPC_ERROR);
    CHECK(peer.eof());
    std::cout << "PASS: final teardown waits for active callback, preserves object lifetime and is "
                 "idempotent\n";
}
void test_concurrent_unregister() {
    Daemon daemon;
    State state;
    RpcService service;
    uint64_t token = register_service(service, daemon, state);
    service.activate_owner(1);
    Peer peer = daemon.accept_peer();
    std::atomic<int> first{99}, second{99};
    std::thread a([&] {
        std::string error;
        first = service.unregister_service(1, token, &error);
    });
    std::thread b([&] {
        std::string error;
        second = service.unregister_service(1, token, &error);
    });
    a.join();
    b.join();
    CHECK(first == DEJAVU_RPC_OK || second == DEJAVU_RPC_OK);
    CHECK(first == DEJAVU_RPC_OK || first == DEJAVU_RPC_ERROR);
    CHECK(second == DEJAVU_RPC_OK || second == DEJAVU_RPC_ERROR);
    CHECK(peer.eof());
    std::cout << "PASS: concurrent unregister cannot double-join or release a running worker\n";
}
void test_validation_and_oversized_input() {
    Daemon daemon;
    State state;
    RpcService service;
    register_service(service, daemon, state);
    service.activate_owner(1);
    Peer peer = daemon.accept_peer();
    peer.send_line(std::string(1024 * 1024 + 1, 'x'));
    CHECK(peer.eof());
    Peer replacement = daemon.accept_peer();
    replacement.send_line(
        "{\"type\":\"request\",\"id\":\"a\\u0000b\",\"route\":\"route\",\"method\":\"echo\"}");
    CHECK(replacement.eof());
    CHECK(state.calls.load() == 0);
    std::cout << "PASS: oversized frames and embedded NUL rejected before business dispatch\n";
}

void test_fragmented_and_coalesced_frames() {
    Daemon daemon;
    State state;
    RpcService service;
    register_service(service, daemon, state);
    service.activate_owner(1);
    Peer peer = daemon.accept_peer(false);
    const std::string hello = "{\"type\":\"hello_ok\",\"version\":1}\n";
    const std::string fragmented = hello + request("fragmented") + "\n";
    // Force incomplete JSON, UTF-free field names, numbers, and newline boundaries.
    const size_t chunks[] = {1, 2, 7, 3, 11};
    size_t offset = 0, chunk = 0;
    while (offset < fragmented.size()) {
        size_t count = std::min(chunks[chunk++ % 5], fragmented.size() - offset);
        peer.send_raw(fragmented.substr(offset, count));
        offset += count;
        if (offset < fragmented.size())
            CHECK(peer.quiet(2));
    }
    Json first(peer.receive());
    CHECK(first.string("id") == "fragmented");
    CHECK(state.calls.load() == 1);
    peer.close_peer();
    Peer coalesced = daemon.accept_peer(false);
    const std::string partial = request("partial");
    size_t split = partial.size() / 2;
    coalesced.send_raw(hello + request("batch-one") + "\n" + request("batch-two") + "\n" +
                       partial.substr(0, split));
    Json one(coalesced.receive()), two(coalesced.receive());
    CHECK(one.string("id") == "batch-one");
    CHECK(two.string("id") == "batch-two");
    CHECK(coalesced.quiet());
    CHECK(state.calls.load() == 3);
    coalesced.send_raw(partial.substr(split));
    CHECK(coalesced.quiet());
    CHECK(state.calls.load() == 3);
    coalesced.send_raw("\n");
    Json last(coalesced.receive());
    CHECK(last.string("id") == "partial");
    CHECK(state.calls.load() == 4);
    CHECK(coalesced.quiet());
    std::cout << "PASS: fragmented handshake/request and coalesced handshake/requests preserve "
                 "frame boundaries\n";
}
void test_same_routes_preserve_active_callback() {
    Daemon daemon;
    State state;
    RpcService service;
    uint64_t token = register_service(service, daemon, state);
    service.activate_owner(1);
    Peer peer = daemon.accept_peer();
    peer.send_line(request("unchanged", "wait"));
    eventually([&] { return state.entered.load(); });
    const char *route = "route";
    std::string error;
    CHECK(service.update_routes(1, token, &route, 1, &error) == DEJAVU_RPC_OK);
    CHECK(peer.quiet(100));
    CHECK(!state.cancelled.load());
    CHECK(!daemon.pending(50));
    state.release.store(true);
    Json response(peer.receive());
    CHECK(response.string("id") == "unchanged");
    CHECK(!state.cancelled.load());
    CHECK(state.calls.load() == 1);
    std::cout
        << "PASS: identical route updates preserve connection and do not cancel active work\n";
}
void test_active_concurrent_teardown() {
    // Repeat independent lifetimes to vary lock acquisition order without sharing test state.
    for (int iteration = 0; iteration < 4; ++iteration) {
        Daemon daemon;
        State state;
        RpcService service;
        uint64_t token = register_service(service, daemon, state);
        service.activate_owner(1);
        Peer peer = daemon.accept_peer();
        peer.send_line(request("active", "ignore"));
        eventually([&] { return state.entered.load(); });
        peer.send_line(request("must-not-run"));
        std::atomic<bool> go{false}, stopped{false};
        std::atomic<int> first{99}, second{99}, updated{99};
        auto unregister = [&](std::atomic<int> &result) {
            while (!go.load())
                std::this_thread::yield();
            std::string error;
            result.store(service.unregister_service(1, token, &error));
        };
        std::thread a([&] { unregister(first); }), b([&] { unregister(second); });
        std::thread update([&] {
            while (!go.load())
                std::this_thread::yield();
            const char *route = "next";
            std::string error;
            updated.store(service.update_routes(1, token, &route, 1, &error));
        });
        std::thread teardown([&] {
            while (!go.load())
                std::this_thread::yield();
            service.stop_and_join();
            stopped.store(true);
        });
        go.store(true);
        eventually(
            [&] { return first.load() != 99 && second.load() != 99 && updated.load() != 99; },
            1500);
        CHECK(first.load() == DEJAVU_RPC_BUSY);
        CHECK(second.load() == DEJAVU_RPC_BUSY);
        CHECK(updated.load() == DEJAVU_RPC_OK || updated.load() == DEJAVU_RPC_ERROR);
        CHECK(!stopped.load());
        CHECK(state.calls.load() == 1);
        a.join();
        b.join();
        update.join();
        CHECK(peer.eof());
        state.release.store(true);
        eventually([&] { return stopped.load(); });
        teardown.join();
        CHECK(state.calls.load() == 1);
        std::string error;
        const char *route = "route";
        CHECK(service.unregister_service(1, token, &error) == DEJAVU_RPC_ERROR);
        CHECK(service.update_routes(1, token, &route, 1, &error) == DEJAVU_RPC_ERROR);
        service.stop_and_join();
        CHECK(state.calls.load() == 1);
    }
    std::cout << "PASS: active callback survives concurrent unregister, route update and final "
                 "drain without queued dispatch\n";
}
void test_callback_self_unregister() {
    Daemon daemon;
    State state;
    uint64_t token = 0;
    std::atomic<int> status{99};
    RpcService *target = nullptr;
    RpcService service([&](uint64_t owner, DejavuRpcHandler callback, void *context,
                           const DejavuRpcRequest *input, DejavuRpcResponse *response, char *error,
                           size_t error_size) {
        std::string reason;
        status.store(target->unregister_service(owner, token, &reason));
        return callback(context, input, response, error, error_size);
    });
    target = &service;
    token = register_service(service, daemon, state);
    service.activate_owner(1);
    Peer peer = daemon.accept_peer();
    peer.send_line(request("self"));
    eventually([&] { return status.load() != 99; });
    CHECK(status.load() == DEJAVU_RPC_BUSY);
    CHECK(peer.eof());
    eventually([&] { return service.retire_owner(1); });
    CHECK(state.calls.load() == 1);
    std::cout << "PASS: callback self-unregister returns busy without self-join and drains on "
                 "owner retry\n";
}

int main() {
    test_lifecycle_validation_and_methods();
    test_zero_routes_and_update();
    test_deadline_and_cancel();
    test_disconnect_and_handshake();
    test_busy_retirement_and_queue();
    test_final_teardown_waits_for_callback();
    test_concurrent_unregister();
    test_validation_and_oversized_input();
    test_fragmented_and_coalesced_frames();
    test_same_routes_preserve_active_callback();
    test_active_concurrent_teardown();
    test_callback_self_unregister();
    std::cout << "OK: RpcService contract and real Unix socket tests\n";
}
