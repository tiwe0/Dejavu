#include "stream_service.h"
#include "ikcp.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
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

uint16_t reserve_loopback_port() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    socklen_t size = sizeof(address);
    assert(getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size) == 0);
    ::close(fd);
    return ntohs(address.sin_port);
}

int connect_loopback(uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    return fd;
}

void send_framed(int fd, const std::string &payload) {
    const uint32_t length = htonl(static_cast<uint32_t>(payload.size()));
    assert(send(fd, &length, sizeof(length), 0) == sizeof(length));
    if (!payload.empty()) {
        assert(send(fd, payload.data(), payload.size(), 0) == static_cast<ssize_t>(payload.size()));
    }
}

std::string receive_framed(int fd) {
    uint32_t length = 0;
    size_t offset = 0;
    while (offset < sizeof(length)) {
        const ssize_t count =
            recv(fd, reinterpret_cast<char *>(&length) + offset, sizeof(length) - offset, 0);
        assert(count > 0);
        offset += static_cast<size_t>(count);
    }
    const size_t size = ntohl(length);
    std::string payload(size, '\0');
    offset = 0;
    while (offset < size) {
        const ssize_t count = recv(fd, payload.data() + offset, size - offset, 0);
        assert(count > 0);
        offset += static_cast<size_t>(count);
    }
    return payload;
}

void listen_accept_read_write() {
    StreamService service;
    const uint16_t port = reserve_loopback_port();
    std::string error;
    const int listener = service.listen(11, DEJAVU_STREAM_TRANSPORT_TCP,
                                        "127.0.0.1:" + std::to_string(port), 4, &error);
    assert(listener > 0 && error.empty());
    const int client = connect_loopback(port);
    const int connection = service.accept(listener, 3000);
    assert(connection > 0);
    send_framed(client, "ping");
    char buffer[64] = {};
    size_t size = 0;
    assert(service.read(connection, buffer, sizeof(buffer), 3000, &size) == 1);
    assert(size == 4 && std::string(buffer, size) == "ping");
    assert(service.write(connection, "pong", 4) == 0);
    assert(receive_framed(client) == "pong");
    assert(service.write(listener, "x", 1) == -1);
    assert(service.close(connection) == 0);
    assert(service.close(listener) == 0);
    ::close(client);
}

void listen_accept_timeout() {
    StreamService service;
    const uint16_t port = reserve_loopback_port();
    std::string error;
    const int listener = service.listen(12, DEJAVU_STREAM_TRANSPORT_TCP,
                                        "127.0.0.1:" + std::to_string(port), 1, &error);
    assert(listener > 0);
    const auto start = Clock::now();
    assert(service.accept(listener, 50) == 0);
    assert(Clock::now() - start < 1500ms);
    assert(service.close_owner(12) == 1);
    assert(service.accept(listener, 10) == -1);
}

void listen_read_timeout_eof_oversize() {
    StreamService service;
    const uint16_t port = reserve_loopback_port();
    std::string error;
    const int listener = service.listen(13, DEJAVU_STREAM_TRANSPORT_TCP,
                                        "127.0.0.1:" + std::to_string(port), 1, &error);
    assert(listener > 0);
    const int client = connect_loopback(port);
    const int connection = service.accept(listener, 3000);
    assert(connection > 0);
    char buffer[8] = {};
    size_t size = 0;
    assert(service.read(connection, buffer, sizeof(buffer), 50, &size) == 0);
    send_framed(client, "x");
    assert(service.read(connection, buffer, sizeof(buffer), 3000, &size) == 1);
    assert(size == 1 && buffer[0] == 'x');
    // Oversized record: the length prefix alone exceeds the caller capacity.
    const uint32_t oversized = htonl(9);
    assert(send(client, &oversized, sizeof(oversized), 0) == sizeof(oversized));
    assert(service.read(connection, buffer, sizeof(buffer), 3000, &size) == -1);
    assert(service.close(connection) == 0);
    assert(service.close(listener) == 0);
    ::close(client);
}

void listen_close_owner_cleans_all() {
    StreamService service;
    const uint16_t port = reserve_loopback_port();
    std::string error;
    const int listener = service.listen(14, DEJAVU_STREAM_TRANSPORT_TCP,
                                        "127.0.0.1:" + std::to_string(port), 1, &error);
    assert(listener > 0);
    const int client = connect_loopback(port);
    const int connection = service.accept(listener, 3000);
    assert(connection > 0);
    assert(service.close_owner(14) == 2);
    assert(service.accept(listener, 10) == -1);
    assert(service.write(connection, "x", 1) == -1);
    ::close(client);
}

void listen_rejects_non_tcp() {
    StreamService service;
    std::string error;
    assert(service.listen(15, DEJAVU_STREAM_TRANSPORT_UDP, "127.0.0.1:1", 1, &error) == 0);
    assert(!error.empty());
}

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

void zero_timeout_accepts_ready_connection() {
    StreamService service;
    const uint16_t port = reserve_loopback_port();
    std::string error;
    const int listener = service.listen(21, DEJAVU_STREAM_TRANSPORT_TCP,
                                        "127.0.0.1:" + std::to_string(port), 4, &error);
    assert(listener > 0);
    const int client = connect_loopback(port);
    int connection = 0;
    const auto deadline = Clock::now() + 500ms;
    do {
        connection = service.accept(listener, 0);
        if (connection == 0)
            std::this_thread::sleep_for(1ms);
    } while (connection == 0 && Clock::now() < deadline);
    assert(connection > 0);
    assert(service.close_owner(21) == 2);
    ::close(client);
}

void zero_timeout_reads_ready_frame() {
    StreamService service;
    const uint16_t port = reserve_loopback_port();
    std::string error;
    const int listener = service.listen(22, DEJAVU_STREAM_TRANSPORT_TCP,
                                        "127.0.0.1:" + std::to_string(port), 4, &error);
    const int client = connect_loopback(port);
    const int connection = service.accept(listener, 3000);
    assert(connection > 0);
    send_framed(client, "ready");
    char buffer[32];
    size_t size = 0;
    // The client send and server delivery can be scheduled separately. Every
    // attempt remains non-blocking, but repeated polls must consume the frame.
    int status = 0;
    const auto deadline = Clock::now() + 500ms;
    do {
        status = service.read(connection, buffer, sizeof(buffer), 0, &size);
        if (status == 0)
            std::this_thread::sleep_for(1ms);
    } while (status == 0 && Clock::now() < deadline);
    assert(status == 1);
    assert(std::string(buffer, size) == "ready");
    assert(service.read(connection, buffer, sizeof(buffer), 0, &size) == 0);
    assert(service.close_owner(22) == 2);
    ::close(client);
}

void close_cancels_accept() {
    StreamService service;
    const uint16_t port = reserve_loopback_port();
    std::string error;
    const int listener = service.listen(23, DEJAVU_STREAM_TRANSPORT_TCP,
                                        "127.0.0.1:" + std::to_string(port), 4, &error);
    std::promise<void> entered;
    auto acceptor = std::async(std::launch::async, [&] {
        entered.set_value();
        return service.accept(listener, -1);
    });
    entered.get_future().wait();
    std::this_thread::sleep_for(20ms);
    assert(service.close(listener) == 0);
    assert(acceptor.wait_for(200ms) == std::future_status::ready);
    assert(acceptor.get() == -1);
}

void close_cancels_read_and_queued_reader() {
    StreamService service;
    const uint16_t port = reserve_loopback_port();
    std::string error;
    const int listener = service.listen(24, DEJAVU_STREAM_TRANSPORT_TCP,
                                        "127.0.0.1:" + std::to_string(port), 4, &error);
    const int client = connect_loopback(port);
    const int connection = service.accept(listener, 3000);
    auto read = [&] {
        char buffer[16];
        size_t size = 0;
        return service.read(connection, buffer, sizeof(buffer), -1, &size);
    };
    auto first = std::async(std::launch::async, read);
    std::this_thread::sleep_for(20ms);
    auto second = std::async(std::launch::async, read);
    assert(service.close(connection) == 0);
    assert(first.wait_for(200ms) == std::future_status::ready);
    assert(second.wait_for(200ms) == std::future_status::ready);
    assert(first.get() == -1 && second.get() == -1);
    assert(service.close_owner(24) == 1);
    ::close(client);
}

void read_timeout_includes_lock_wait() {
    StreamService service;
    const uint16_t port = reserve_loopback_port();
    std::string error;
    const int listener = service.listen(25, DEJAVU_STREAM_TRANSPORT_TCP,
                                        "127.0.0.1:" + std::to_string(port), 4, &error);
    const int client = connect_loopback(port);
    const int connection = service.accept(listener, 3000);
    auto first = std::async(std::launch::async, [&] {
        char buffer[16];
        size_t size = 0;
        return service.read(connection, buffer, sizeof(buffer), -1, &size);
    });
    std::this_thread::sleep_for(20ms);
    const auto start = Clock::now();
    auto second = std::async(std::launch::async, [&] {
        char buffer[16];
        size_t size = 0;
        return service.read(connection, buffer, sizeof(buffer), 30, &size);
    });
    assert(second.wait_for(150ms) == std::future_status::ready);
    assert(second.get() == 0);
    assert(Clock::now() - start < 150ms);
    assert(service.close_owner(25) == 2);
    assert(first.wait_for(200ms) == std::future_status::ready);
    assert(first.get() == -1);
    ::close(client);
}

void close_owner_racing_accept_leaves_no_connection() {
    for (int iteration = 0; iteration < 40; ++iteration) {
        StreamService service;
        const uint16_t port = reserve_loopback_port();
        std::string error;
        const int listener = service.listen(26, DEJAVU_STREAM_TRANSPORT_TCP,
                                            "127.0.0.1:" + std::to_string(port), 4, &error);
        assert(listener > 0);
        const int client = connect_loopback(port);
        auto acceptor =
            std::async(std::launch::async, [&] { return service.accept(listener, -1); });
        const size_t count = service.close_owner(26);
        assert(count >= 1 && count <= 2);
        assert(acceptor.wait_for(200ms) == std::future_status::ready);
        const int connection = acceptor.get();
        if (connection > 0)
            assert(service.write(connection, "late", 4) == -1);
        assert(service.close_owner(26) == 0);
        ::close(client);
    }
}

void stream_owner_is_checked_with_handle_lookup() {
    StreamService service;
    const uint16_t port = reserve_loopback_port();
    std::string error;
    const int listener = service.listen(27, DEJAVU_STREAM_TRANSPORT_TCP,
                                        "127.0.0.1:" + std::to_string(port), 4, &error);
    const int client = connect_loopback(port);
    assert(service.accept(listener, 0, 28) == -1);
    assert(service.close(listener, 28) == -1);
    const int connection = service.accept(listener, 3000, 27);
    assert(connection > 0 && service.owned_by(connection, 27));
    send_framed(client, "owned");
    char buffer[32];
    size_t size = 99;
    assert(service.read(connection, buffer, sizeof(buffer), 0, &size, 28) == -1);
    assert(size == 0);
    assert(service.write(connection, "wrong", 5, 28) == -1);
    assert(service.close(connection, 28) == -1);
    assert(service.read(connection, buffer, sizeof(buffer), 3000, &size, 27) == 1);
    assert(std::string(buffer, size) == "owned");
    assert(service.write(connection, "right", 5, 27) == 0);
    assert(receive_framed(client) == "right");
    assert(service.close(connection, 27) == 0);
    assert(service.close(listener, 27) == 0);
    ::close(client);
}

void retired_owner_rejects_new_streams() {
    StreamService service;
    const uint16_t port = reserve_loopback_port();
    std::string error;
    const std::string endpoint = "127.0.0.1:" + std::to_string(port);
    const int listener = service.listen(28, DEJAVU_STREAM_TRANSPORT_TCP, endpoint, 4, &error);
    assert(listener > 0);
    auto acceptor =
        std::async(std::launch::async, [&] { return service.accept(listener, -1, 28); });
    std::this_thread::sleep_for(20ms);
    assert(service.retire_owner(28) == 1);
    assert(acceptor.wait_for(200ms) == std::future_status::ready);
    assert(acceptor.get() == -1);
    assert(service.listen(28, DEJAVU_STREAM_TRANSPORT_TCP, endpoint, 4, &error) == 0);
    assert(error == "stream owner is retired");
    for (uint32_t transport : {DEJAVU_STREAM_TRANSPORT_UDP, DEJAVU_STREAM_TRANSPORT_KCP}) {
        error.clear();
        assert(service.open(28, transport, endpoint, &error) == 0);
        assert(error == "stream owner is retired");
    }
    assert(service.close_owner(28) == 0);
    // close_owner is intentionally reusable; only retire_owner revokes the token.
    assert(service.listen(29, DEJAVU_STREAM_TRANSPORT_TCP, endpoint, 4, &error) > 0);
    assert(service.close_owner(29) == 1);
    assert(service.listen(29, DEJAVU_STREAM_TRANSPORT_TCP, endpoint, 4, &error) > 0);
    assert(service.close_owner(29) == 1);
}

void retire_owner_racing_open_does_not_publish_late_stream() {
    for (int iteration = 0; iteration < 30; ++iteration) {
        StreamService service;
        Peer peer;
        auto opener = std::async(std::launch::async, [&] {
            std::string error;
            return service.open(30, DEJAVU_STREAM_TRANSPORT_TCP, peer.endpoint, &error);
        });
        service.retire_owner(30);
        const int handle = opener.get();
        if (handle > 0)
            assert(service.write(handle, "late", 4, 30) == -1);
        assert(service.close_owner(30) == 0);
    }
}

void read_preserves_partial_frames_and_eof() {
    StreamService service;
    const uint16_t port = reserve_loopback_port();
    std::string error;
    const int listener = service.listen(31, DEJAVU_STREAM_TRANSPORT_TCP,
                                        "127.0.0.1:" + std::to_string(port), 4, &error);
    const int client = connect_loopback(port);
    const int connection = service.accept(listener, 3000);
    const uint32_t length = htonl(5);
    assert(send(client, &length, sizeof(length), 0) == sizeof(length));
    assert(send(client, "he", 2, 0) == 2);
    char buffer[32];
    size_t size = 0;
    assert(service.read(connection, buffer, sizeof(buffer), 20, &size) == 0);
    assert(send(client, "llo", 3, 0) == 3);
    ::close(client);
    assert(service.read(connection, buffer, sizeof(buffer), 3000, &size) == 1);
    assert(std::string(buffer, size) == "hello");
    assert(service.read(connection, buffer, sizeof(buffer), 3000, &size) == -1);
    assert(size == 0);
    assert(service.close_owner(31) == 2);
}

void udp_framing_survives_lifecycle_changes() {
    const int receiver = socket(AF_INET, SOCK_DGRAM, 0);
    assert(receiver >= 0);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(receiver, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    assert(getsockname(receiver, reinterpret_cast<sockaddr *>(&address), &length) == 0);
    StreamService service;
    std::string error;
    const int handle = service.open(32, DEJAVU_STREAM_TRANSPORT_UDP,
                                    "127.0.0.1:" + std::to_string(ntohs(address.sin_port)), &error);
    assert(handle > 0);
    assert(service.write(handle, "udp", 3, 32) == 0);
    char frame[32];
    assert(recv(receiver, frame, sizeof(frame), 0) == 7);
    uint32_t payload_length = 0;
    std::memcpy(&payload_length, frame, 4);
    assert(ntohl(payload_length) == 3 && std::string(frame + 4, 3) == "udp");
    assert(service.retire_owner(32) == 1);
    assert(service.write(handle, "late", 4, 32) == -1);
    ::close(receiver);
}

void kcp_framing_survives_lifecycle_changes() {
    const int receiver = socket(AF_INET, SOCK_DGRAM, 0);
    assert(receiver >= 0);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(receiver, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    assert(getsockname(receiver, reinterpret_cast<sockaddr *>(&address), &length) == 0);
    StreamService service;
    std::string error;
    const int handle = service.open(33, DEJAVU_STREAM_TRANSPORT_KCP,
                                    "127.0.0.1:" + std::to_string(ntohs(address.sin_port)), &error);
    assert(handle > 0);
    assert(service.write(handle, "kcp", 3, 33) == 0);
    char packet[2048];
    const ssize_t size = recv(receiver, packet, sizeof(packet), 0);
    assert(size >= 24);
    ikcpcb *peer = ikcp_create(ikcp_getconv(packet), nullptr);
    assert(peer != nullptr && ikcp_input(peer, packet, size) == 0);
    char frame[32];
    assert(ikcp_recv(peer, frame, sizeof(frame)) == 7);
    uint32_t payload_length = 0;
    std::memcpy(&payload_length, frame, 4);
    assert(ntohl(payload_length) == 3 && std::string(frame + 4, 3) == "kcp");
    ikcp_release(peer);
    assert(service.retire_owner(33) == 1);
    assert(service.write(handle, "late", 4, 33) == -1);
    ::close(receiver);
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
    passed = run_test("TCP listen/accept/framed read/write", listen_accept_read_write) && passed;
    passed = run_test("accept timeout", listen_accept_timeout) && passed;
    passed =
        run_test("read timeout/EOF/oversized record", listen_read_timeout_eof_oversize) && passed;
    passed =
        run_test("close_owner covers listener and connections", listen_close_owner_cleans_all) &&
        passed;
    passed = run_test("listen rejects non-TCP transports", listen_rejects_non_tcp) && passed;
    passed = run_test("zero timeout accepts a ready connection",
                      zero_timeout_accepts_ready_connection) &&
             passed;
    passed = run_test("zero timeout reads a ready frame", zero_timeout_reads_ready_frame) && passed;
    passed = run_test("close cancels indefinite accept", close_cancels_accept) && passed;
    passed =
        run_test("close cancels active and queued readers", close_cancels_read_and_queued_reader) &&
        passed;
    passed =
        run_test("read timeout includes read lock wait", read_timeout_includes_lock_wait) && passed;
    passed = run_test("owner close racing accept leaves no live connection",
                      close_owner_racing_accept_leaves_no_connection) &&
             passed;
    passed = run_test("all stream operations enforce the expected owner",
                      stream_owner_is_checked_with_handle_lookup) &&
             passed;
    passed = run_test("owner retirement rejects new streams", retired_owner_rejects_new_streams) &&
             passed;
    passed = run_test("owner retirement rejects in-progress opens",
                      retire_owner_racing_open_does_not_publish_late_stream) &&
             passed;
    passed = run_test("partial frames survive timeout and drain before EOF",
                      read_preserves_partial_frames_and_eof) &&
             passed;
    passed = run_test("UDP framing remains compatible", udp_framing_survives_lifecycle_changes) &&
             passed;
    passed = run_test("KCP framing remains compatible", kcp_framing_survives_lifecycle_changes) &&
             passed;
    return passed ? 0 : 1;
}
