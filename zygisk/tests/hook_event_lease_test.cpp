#include "hook_event_sink.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <limits>

struct Peer {
    int listener = -1;
    int connection = -1;
    std::string endpoint;
    Peer() {
        listener = socket(AF_INET, SOCK_STREAM, 0);
        assert(listener >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
        assert(listen(listener, 4) == 0);
        socklen_t size = sizeof(address);
        assert(getsockname(listener, reinterpret_cast<sockaddr *>(&address), &size) == 0);
        endpoint = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    }
    void accept_client() {
        if (connection >= 0) close(connection);
        connection = accept(listener, nullptr, nullptr);
        assert(connection >= 0);
    }
    ~Peer() { if (connection >= 0) close(connection); if (listener >= 0) close(listener); }
};

int main() {
    alarm(8);
    auto &sink = HookEventSink::instance();
    Peer first, second;
    std::string error;
    assert(sink.start(first.endpoint, &error));
    first.accept_client();
    uint64_t lease = 99;
    assert(!sink.acquire("invalid endpoint", &lease, &error) && lease == 0);
    assert(!sink.acquire(second.endpoint, &lease, &error) && lease == 0);
    assert(sink.active() && sink.stats().find(first.endpoint) != std::string::npos);
    assert(sink.emit("t", "x", 1));
    char frame[8]{};
    size_t received = 0;
    while (received < sizeof(frame)) {
        ssize_t count = recv(first.connection, frame + received, sizeof(frame) - received, 0);
        assert(count > 0);
        received += static_cast<size_t>(count);
    }
    assert(std::string(frame, sizeof(frame)) == std::string("\0\0\0\4\0\1tx", 8));
    sink.stop();
    std::puts("OK: invalid or busy acquire leaves the existing stream usable");

    uint64_t old_lease = 0, new_lease = 0;
    assert(sink.acquire(first.endpoint, &old_lease, &error) && old_lease > 0);
    first.accept_client();
    assert(!sink.acquire(second.endpoint, &new_lease, &error) && new_lease == 0);
    assert(sink.release(old_lease));
    assert(sink.acquire(second.endpoint, &new_lease, &error) && new_lease > old_lease);
    second.accept_client();
    assert(!sink.release(old_lease));
    assert(!sink.release(0));
    assert(!sink.release(std::numeric_limits<uint64_t>::max()));
    assert(sink.active() && sink.stats().find(second.endpoint) != std::string::npos);
    assert(sink.start(first.endpoint, &error));
    first.accept_client();
    assert(!sink.release(new_lease) && sink.active());
    sink.stop();
    std::puts("OK: stale releases cannot stop a replacement managed or legacy channel");

    std::string unavailable;
    { Peer closed; unavailable = closed.endpoint; }
    assert(sink.acquire(unavailable, &lease, &error));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (sink.active() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(!sink.active());
    uint64_t blocked = 0;
    assert(!sink.acquire(second.endpoint, &blocked, &error) && blocked == 0);
    assert(sink.release(lease));
    std::puts("OK: failed connections retain their lease until their owner releases it");

    uint64_t leases[2]{};
    bool results[2]{};
    std::thread workers[2];
    for (int i = 0; i < 2; ++i) workers[i] = std::thread([&, i] {
        std::string failure;
        results[i] = sink.acquire(first.endpoint, &leases[i], &failure);
    });
    for (auto &worker : workers) worker.join();
    assert(results[0] != results[1]);
    first.accept_client();
    assert(sink.release(results[0] ? leases[0] : leases[1]));
    std::puts("OK: concurrent managed acquire has exactly one owner");
}
