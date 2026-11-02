#include "stream_service.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <future>
#include <string>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct Peer {
    int listener = -1;
    int client = -1;
    std::string endpoint;

    Peer() {
        listener = socket(AF_INET, SOCK_STREAM, 0);
        assert(listener >= 0);
        const int receive_buffer = 4096;
        assert(setsockopt(listener, SOL_SOCKET, SO_RCVBUF, &receive_buffer,
                          sizeof(receive_buffer)) == 0);
        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
        assert(listen(listener, 1) == 0);
        socklen_t size = sizeof(address);
        assert(getsockname(listener, reinterpret_cast<sockaddr *>(&address), &size) == 0);
        endpoint = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    }

    ~Peer() {
        if (client >= 0) {
            ::close(client);
        }
        ::close(listener);
    }

    void accept_client() {
        client = accept(listener, nullptr, nullptr);
        assert(client >= 0);
    }

    void read_exact(void *buffer, size_t size) {
        char *bytes = static_cast<char *>(buffer);
        while (size != 0) {
            const ssize_t count = recv(client, bytes, size, 0);
            assert(count > 0);
            size -= static_cast<size_t>(count);
            bytes += count;
        }
    }
};

int open_stream(StreamService &service, Peer &peer) {
    std::string error;
    const int handle = service.open(7, DEJAVU_STREAM_TRANSPORT_TCP, peer.endpoint, &error);
    assert(handle > 0 && error.empty());
    peer.accept_client();
    return handle;
}

void normal_frames() {
    Peer peer;
    StreamService service;
    const int handle = open_stream(service, peer);
    assert(service.write(handle, "hello", 5) == 0);
    assert(service.write(handle, nullptr, 0) == 0);
    uint32_t length = 0;
    peer.read_exact(&length, sizeof(length));
    assert(ntohl(length) == 5);
    char payload[5];
    peer.read_exact(payload, sizeof(payload));
    assert(std::string(payload, sizeof(payload)) == "hello");
    peer.read_exact(&length, sizeof(length));
    assert(ntohl(length) == 0);
    assert(service.close_owner(7) == 1);
    assert(service.write(handle, "late", 4) == -1);
}

void stopped_reader_times_out() {
    Peer peer;
    StreamService service;
    const int handle = open_stream(service, peer);
    const std::string payload(32 * 1024 * 1024, 'x');
    const auto start = Clock::now();
    assert(service.write(handle, payload.data(), payload.size()) == -1);
    assert(Clock::now() - start < 1500ms);
    // A partially written frame must poison the stream instead of allowing a
    // later record to be interpreted as the missing bytes of the first one.
    assert(service.write(handle, "next", 4) == -1);
    assert(service.close(handle) == 0);
}

void close_cancels_writer() {
    Peer peer;
    StreamService service;
    const int handle = open_stream(service, peer);
    const std::string payload(32 * 1024 * 1024, 'x');
    auto writer = std::async(std::launch::async,
                             [&] { return service.write(handle, payload.data(), payload.size()); });
    char byte = 0;
    peer.read_exact(&byte, 1); // The writer is inside its send loop.
    const auto start = Clock::now();
    assert(service.close(handle) == 0);
    assert(writer.wait_for(80ms) == std::future_status::ready);
    assert(writer.get() == -1);
    assert(Clock::now() - start < 80ms);
    assert(service.close(handle) == -1);
}

bool run_test(const char *name, void (*test)()) {
    std::fflush(nullptr);
    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        alarm(3); // Regressions must fail instead of hanging the host suite.
        test();
        _exit(0);
    }
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    const bool passed = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::printf("%s: %s\n", passed ? "OK" : "FAIL", name);
    return passed;
}

} // namespace

int main() {
    bool passed = run_test("TCP framing and owner cleanup", normal_frames);
    passed = run_test("stopped TCP reader has bounded write and terminal failure",
                      stopped_reader_times_out) &&
             passed;
    passed = run_test("close interrupts an active TCP writer", close_cancels_writer) && passed;
    return passed ? 0 : 1;
}
