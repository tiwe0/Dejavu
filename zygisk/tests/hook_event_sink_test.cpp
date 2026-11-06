#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>
#include <string>
using namespace std::chrono_literals;
namespace {
std::atomic<bool> stalled_connect{false};
std::atomic<bool> connect_waiting{false};
std::atomic<bool> stalled_send{false};
std::mutex send_mutex;
std::condition_variable send_condition;
bool send_waiting = false;
int test_connect(int fd, const sockaddr *address, socklen_t length) {
    if (stalled_connect.load()) { errno = EINPROGRESS; return -1; }
    return ::connect(fd, address, length);
}
[[maybe_unused]] int test_poll(pollfd *descriptors, nfds_t count, int timeout) {
    if (stalled_connect.load()) {
        connect_waiting.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout, 10)));
        return 0;
    }
    if (stalled_send.load()) {
        std::unique_lock<std::mutex> lock(send_mutex);
        send_waiting = true;
        send_condition.notify_all();
        send_condition.wait_for(lock, std::chrono::milliseconds(timeout), [] {
            return !stalled_send.load();
        });
        return 0;
    }
    return ::poll(descriptors, count, timeout);
}
ssize_t test_send(int fd, const void *data, size_t size, int flags) {
    if (stalled_send.load()) { errno = EAGAIN; return -1; }
    return ::send(fd, data, size, flags);
}
}
#define connect test_connect
#define poll test_poll
#define send test_send
#include "../src/hook_event_sink.cpp"
#undef connect
#undef poll
#undef send
namespace {
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
        assert(::listen(listener, 4) == 0);
        socklen_t length = sizeof(address);
        assert(getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) == 0);
        endpoint = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    }
    void accept_client() { connection = ::accept(listener, nullptr, nullptr); assert(connection >= 0); }
    void close_client() { if (connection >= 0) { ::close(connection); connection = -1; } }
    ~Peer() { close_client(); ::close(listener); }
};
void normal_frame() {
    Peer peer;
    auto &sink = HookEventSink::instance();
    std::string error;
    assert(sink.start(peer.endpoint, &error));
    peer.accept_client();
    assert(sink.emit("topic", "hello", 5));
    char bytes[16]{};
    size_t offset = 0;
    while (offset < sizeof(bytes)) {
        const auto size = recv(peer.connection, bytes + offset, sizeof(bytes) - offset, 0);
        assert(size > 0); offset += static_cast<size_t>(size);
    }
    assert(std::string(bytes, sizeof(bytes)) == std::string("\0\0\0\x0c\0\x05topichello", 16));
    sink.stop();
    assert(!sink.active());
}
#if defined(DEJAVU_EVENT_CANCELLATION_PROBE)
void stop_backpressure() {
    Peer peer;
    auto &sink = HookEventSink::instance();
    std::string error;
    assert(sink.start(peer.endpoint, &error));
    peer.accept_client();
    // This variant has a five-second send deadline. A short stop must therefore
    // come from cancellation, never the normal production 100 ms send timeout.
    static_assert(kSendTimeoutMs >= 5000, "cancellation probe needs a long send deadline");
    {
        std::lock_guard<std::mutex> lock(send_mutex);
        send_waiting = false;
    }
    stalled_send.store(true);
    assert(sink.emit("test", "blocked", 7));
    {
        std::unique_lock<std::mutex> lock(send_mutex);
        assert(send_condition.wait_for(lock, 1s, [] { return send_waiting; }));
    }
    auto stopped = std::async(std::launch::async, [&] { sink.stop(); });
    const auto status = stopped.wait_for(250ms);
    // Also release a deliberately mutated worker, so the failure is an
    // assertion rather than an unexplained test-suite hang.
    stalled_send.store(false);
    send_condition.notify_all();
    peer.close_client();
    stopped.get();
    assert(status == std::future_status::ready);
    assert(!sink.active());
}
#endif
void stop_pending_connect() {
    stalled_connect.store(true);
    connect_waiting.store(false);
    auto &sink = HookEventSink::instance();
    std::string error;
    assert(sink.start("127.0.0.1:1", &error));
    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (!connect_waiting.load() && sink.active() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    assert(connect_waiting.load());
    const auto start = std::chrono::steady_clock::now();
    sink.stop();
    assert(std::chrono::steady_clock::now() - start < 250ms);
    stalled_connect.store(false);
}
void pending_connect_expires() {
    stalled_connect.store(true);
    connect_waiting.store(false);
    auto &sink = HookEventSink::instance();
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    assert(sink.start("127.0.0.1:1", &error));
    while (sink.active() && std::chrono::steady_clock::now() - start < 3500ms)
        std::this_thread::sleep_for(2ms);
    assert(!sink.active());
    assert(std::chrono::steady_clock::now() - start < 3500ms);
    assert(sink.stats().find("connect_failures=1") != std::string::npos);
    sink.stop();
    stalled_connect.store(false);
}
void stalled_send_expires() {
    Peer peer;
    auto &sink = HookEventSink::instance();
    std::string error;
    assert(sink.start(peer.endpoint, &error));
    peer.accept_client();
    const int receive_size = 1024;
    setsockopt(peer.connection, SOL_SOCKET, SO_RCVBUF, &receive_size, sizeof(receive_size));
    std::string payload(4u * 1024u * 1024u, 'x');
    assert(sink.emit("test", payload.data(), payload.size()));
    const auto start = std::chrono::steady_clock::now();
    while (sink.active() && std::chrono::steady_clock::now() - start < 1000ms)
        std::this_thread::sleep_for(2ms);
    assert(!sink.active());
    assert(sink.stats().find("send_failures=1") != std::string::npos);
    assert(sink.stats().find("queued=0") != std::string::npos);
    sink.stop();
}
void concurrent_start_stop() {
    stalled_connect.store(true);
    auto &sink = HookEventSink::instance();
    auto exercise = [&] {
        for (int i = 0; i < 20; ++i) {
            std::string error;
            assert(sink.start("127.0.0.1:1", &error));
            sink.stop();
        }
    };
    std::thread first(exercise), second(exercise);
    first.join(); second.join();
    sink.stop();
    assert(!sink.active());
    stalled_connect.store(false);
}
void start_stalled_sink(HookEventSink &sink) {
    stalled_connect.store(true);
    connect_waiting.store(false);
    std::string error;
    assert(sink.start("127.0.0.1:1", &error));
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!connect_waiting.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    assert(connect_waiting.load() && sink.active());
}
void queue_frame_limit_and_recovery() {
    auto &sink = HookEventSink::instance();
    start_stalled_sink(sink);
    for (size_t i = 0; i < 2048; ++i) assert(sink.emit("q", "x", 1));
    assert(!sink.emit("q", "overflow", 8));
    assert(sink.stats().find("queued=2048 ") != std::string::npos);
    sink.stop();
    assert(sink.stats().find("queued=0 ") != std::string::npos);
    stalled_connect.store(false);
    normal_frame(); // Restart must deliver only the new event, not a stale queued frame.
}
void queue_byte_limit_and_recovery() {
    auto &sink = HookEventSink::instance();
    start_stalled_sink(sink);
    const std::string payload(4u * 1024u * 1024u, 'b');
    assert(sink.emit("q", payload.data(), payload.size()));
    assert(!sink.emit("q", payload.data(), payload.size()));
    // Two records include six bytes of topic metadata; fill exactly 8 MiB.
    assert(sink.emit("q", payload.data(), payload.size() - 6));
    assert(!sink.emit("q", "", 0));
    sink.stop();
    start_stalled_sink(sink);
    assert(sink.emit("q", payload.data(), payload.size()));
    sink.stop();
    stalled_connect.store(false);
    normal_frame();
}
void failed_connection_can_restart() {
    Peer unavailable;
    ::close(unavailable.listener);
    unavailable.listener = -1;
    auto &sink = HookEventSink::instance();
    std::string error;
    assert(sink.start(unavailable.endpoint, &error));
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (sink.active() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    assert(!sink.active());
    assert(sink.stats().find("connect_failures=1") != std::string::npos);
    normal_frame();
}
void failed_send_can_restart() {
    stalled_send_expires();
    normal_frame();
}
bool run(const char *name, void (*test)()) {
    std::fflush(nullptr);
    const auto pid = fork(); assert(pid >= 0);
    if (pid == 0) { alarm(5); test(); _exit(0); }
    int status = 0; assert(waitpid(pid, &status, 0) == pid);
    const bool passed = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::printf("%s: %s\n", passed ? "OK" : "FAIL", name);
    return passed;
}
}
int main(int argc, char **) {
#if defined(DEJAVU_EVENT_CANCELLATION_PROBE)
    if (argc > 1)
        return run("stop cancels a blocked send before its five-second deadline", stop_backpressure) ? 0 : 1;
#else
    (void)argc;
#endif
    bool passed = run("event frames preserve wire format", normal_frame);
    passed = run("stop cancels a pending event connection", stop_pending_connect) && passed;
    passed = run("pending event connections have a deadline", pending_connect_expires) && passed;
    passed = run("stalled event sends have a deadline", stalled_send_expires) && passed;
    passed = run("concurrent event starts/stops preserve worker ownership", concurrent_start_stop) && passed;
    passed = run("frame queue limit resets on stop and restart", queue_frame_limit_and_recovery) && passed;
    passed = run("byte queue limit resets on stop and restart", queue_byte_limit_and_recovery) && passed;
    passed = run("a failed connection can restart and send", failed_connection_can_restart) && passed;
    passed = run("a failed send can restart and send", failed_send_can_restart) && passed;
    return passed ? 0 : 1;
}
