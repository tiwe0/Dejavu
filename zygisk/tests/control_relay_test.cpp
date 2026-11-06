#include "control_channel.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void pair(int (&fds)[2]) {
    require(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
}

void wait_ok(pid_t child) {
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    require(waited == child, "waitpid");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::cerr << "child status: " << status << '\n';
        require(false, "watchdog child failed or exceeded bound");
    }
}

void bounded(void (*test)()) {
    const pid_t child = fork();
    require(child >= 0, "fork watchdog");
    if (child == 0) {
        alarm(3);
        test();
        _exit(0);
    }
    wait_ok(child);
}

void deadlines() {
    int fds[2];
    pair(fds);
    std::string received;
    auto start = Clock::now();
    require(!dejavu_control_read_frame(fds[0], &received, 1024, start + 80ms) &&
                errno == ETIMEDOUT && Clock::now() - start < 500ms,
            "idle read deadline");
    require(dejavu_control_write_frame(fds[1], "x", 1), "queue frame");
    require(!dejavu_control_read_frame(fds[0], &received, 1024, Clock::now() - 1ms) &&
                errno == ETIMEDOUT,
            "expired deadline rejects queued frame");
    require(dejavu_control_peer_is_root(fds[0]) == (getuid() == 0), "peer credentials");
    close(fds[0]);
    close(fds[1]);
    DejavuControlEndpoint endpoint = {};
    std::strcpy(endpoint.socket_name, "dejavu-expired-connect");
    endpoint.token[0] = 1;
    require(dejavu_control_connect(endpoint, Clock::now() - 1ms) == -1 && errno == ETIMEDOUT,
            "expired connect rejected");
    require(!dejavu_control_peer_is_root(-1), "invalid peer rejected");

    pair(fds);
    int small_buffer = 1024;
    require(setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &small_buffer, sizeof(small_buffer)) == 0,
            "small send buffer");
    const std::string large(kDejavuMaximumControlScriptSize, 'x');
    start = Clock::now();
    require(!dejavu_control_write_frame(fds[0], large.data(), large.size(), start + 80ms) &&
                errno == ETIMEDOUT && Clock::now() - start < 500ms,
            "stalled send deadline");
    close(fds[0]);
    close(fds[1]);

    pair(fds);
    close(fds[1]);
    require(!dejavu_control_write_frame(fds[0], "x", 1, Clock::now() + 80ms),
            "closed peer send survives SIGPIPE");
    close(fds[0]);
}

void partial_frame() {
    int fds[2];
    pair(fds);
    const pid_t writer = fork();
    require(writer >= 0, "fork slow writer");
    if (writer == 0) {
        close(fds[0]);
        const uint32_t size = 2;
        const char *header = reinterpret_cast<const char *>(&size);
        // Each byte arrives before a per-read timeout, but the whole header
        // and body cannot fit within the single absolute budget.
        for (size_t i = 0; i < sizeof(size); ++i) {
            usleep(20000);
            if (send(fds[1], header + i, 1, 0) != 1)
                _exit(1);
        }
        usleep(50000);
        if (send(fds[1], "ab", 2, 0) != 2)
            _exit(1);
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    std::string payload;
    const auto start = Clock::now();
    require(!dejavu_control_read_frame(fds[0], &payload, 1024, start + 110ms) &&
                errno == ETIMEDOUT && Clock::now() - start < 400ms,
            "partial header and body share absolute deadline");
    wait_ok(writer);
    close(fds[0]);
}

void listener_connect() {
#if defined(__linux__)
    DejavuControlEndpoint endpoint = {};
    const int listener = dejavu_control_open_listener(&endpoint);
    require(listener >= 0, "open random endpoint listener");
    require((fcntl(listener, F_GETFL) & O_NONBLOCK) != 0, "listener is nonblocking");
    const int client = dejavu_control_connect(endpoint, Clock::now() + 100ms);
    require(client >= 0, "bounded abstract connect");
    const int peer = accept(listener, nullptr, nullptr);
    require(peer >= 0, "accept bounded connect");
    require(dejavu_control_peer_is_root(client) == (getuid() == 0), "server credentials");
    require(dejavu_control_write_authentication(client, endpoint, Clock::now() + 100ms) &&
                dejavu_control_read_authentication(peer, endpoint, Clock::now() + 100ms),
            "generated endpoint authenticates");
    close(peer);
    close(client);
    close(listener);
    require(dejavu_control_connect(endpoint, Clock::now() + 100ms) < 0,
            "closed listener connect fails");
#endif
}

void ignore_signal(int) {
}

void interrupted_deadline() {
    struct sigaction action = {};
    action.sa_handler = ignore_signal;
    sigemptyset(&action.sa_mask);
    require(sigaction(SIGUSR1, &action, nullptr) == 0, "signal handler");
    int fds[2];
    pair(fds);
    const pid_t notifier = fork();
    require(notifier >= 0, "fork interrupter");
    if (notifier == 0) {
        const pid_t reader = getppid();
        for (int i = 0; i < 8; ++i) {
            usleep(15000);
            if (kill(reader, SIGUSR1) != 0)
                _exit(1);
        }
        _exit(0);
    }
    std::string payload;
    const auto start = Clock::now();
    require(!dejavu_control_read_frame(fds[0], &payload, 16, start + 80ms) && errno == ETIMEDOUT &&
                Clock::now() - start < 160ms,
            "EINTR preserves absolute deadline");
    wait_ok(notifier);
    close(fds[0]);
    close(fds[1]);
}

void nested_rpc_deadline() {
    int encoded[2];
    pair(encoded);
    const DejavuControlRpcMessage request = {DejavuControlRpcType::kExecuteRequest, 1,
                                             DEJAVU_CONTROL_RPC_OK, "abc"};
    require(dejavu_control_write_rpc(encoded[0], request), "encode nested RPC");
    char bytes[4 + 28 + 3] = {};
    require(recv(encoded[1], bytes, sizeof(bytes), MSG_WAITALL) == sizeof(bytes), "encoded RPC");
    close(encoded[0]);
    close(encoded[1]);
    int fds[2];
    pair(fds);
    const pid_t writer = fork();
    require(writer >= 0, "fork nested RPC writer");
    if (writer == 0) {
        close(fds[0]);
        usleep(50000);
        if (send(fds[1], bytes, 4, 0) != 4)
            _exit(1);
        usleep(40000);
        if (send(fds[1], bytes + 4, 28, 0) != 28)
            _exit(1);
        usleep(60000);
        if (send(fds[1], bytes + 32, 3, 0) != 3)
            _exit(1);
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    DejavuControlRpcMessage decoded;
    const auto start = Clock::now();
    require(!dejavu_control_read_rpc(fds[0], &decoded, start + 120ms) && errno == ETIMEDOUT &&
                Clock::now() - start < 250ms,
            "outer frame and nested RPC share deadline");
    wait_ok(writer);
    close(fds[0]);
}

void malformed_and_eof() {
    int fds[2];
    std::string payload = "unchanged";
    pair(fds);
    const uint32_t excessive = 4096;
    require(send(fds[1], &excessive, sizeof(excessive), 0) == sizeof(excessive), "size send");
    require(!dejavu_control_read_frame(fds[0], &payload, 16, Clock::now() + 80ms) &&
                payload == "unchanged",
            "oversized frame rejected before allocation");
    close(fds[0]);
    close(fds[1]);
    pair(fds);
    const uint32_t size = 3;
    require(send(fds[1], &size, 2, 0) == 2, "partial header send");
    close(fds[1]);
    require(!dejavu_control_read_frame(fds[0], &payload, 16, Clock::now() + 80ms),
            "truncated header rejected");
    close(fds[0]);
    pair(fds);
    require(send(fds[1], &size, sizeof(size), 0) == sizeof(size) && send(fds[1], "a", 1, 0) == 1,
            "partial body send");
    close(fds[1]);
    require(!dejavu_control_read_frame(fds[0], &payload, 16, Clock::now() + 80ms),
            "truncated body rejected");
    close(fds[0]);
    pair(fds);
    require(dejavu_control_write_frame(fds[1], "abc", 3), "HUP buffered frame");
    close(fds[1]);
    require(dejavu_control_read_frame(fds[0], &payload, 16, Clock::now() + 80ms) &&
                payload == "abc",
            "buffered data survives peer HUP");
    require(!dejavu_control_read_frame(fds[0], &payload, 16, Clock::now() + 80ms),
            "clean EOF rejected");
    close(fds[0]);
}

constexpr char kUnknown[] =
    "RPC execution outcome unknown; request may have executed; do not retry automatically";

void relay() {
    const DejavuControlRpcMessage request = {DejavuControlRpcType::kExecuteRequest, 17,
                                             DEJAVU_CONTROL_RPC_OK, "return 42"};
    DejavuControlRpcMessage response;
    int agent = -1;
    require(dejavu_control_relay_rpc(&agent, request, &response, Clock::now() + 80ms) &&
                response.status == DEJAVU_CONTROL_RPC_UNAVAILABLE &&
                response.payload == "target Agent is not connected",
            "predispatch unavailable");
    int fds[2];
    pair(fds);
    agent = fds[0];
    const pid_t peer = fork();
    require(peer >= 0, "fork lost reply");
    if (peer == 0) {
        close(fds[0]);
        DejavuControlRpcMessage decoded;
        require(dejavu_control_read_rpc(fds[1], &decoded, Clock::now() + 500ms) &&
                    decoded.request_id == request.request_id,
                "dispatch arrived");
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    require(dejavu_control_relay_rpc(&agent, request, &response, Clock::now() + 500ms) &&
                agent == -1 && response.request_id == request.request_id &&
                response.status == DEJAVU_CONTROL_RPC_UNAVAILABLE && response.payload == kUnknown,
            "lost reply has unknown outcome without retry");
    wait_ok(peer);

    // A new connection remains usable after the failed request is discarded.
    pair(fds);
    agent = fds[0];
    const DejavuControlRpcMessage reply = {DejavuControlRpcType::kExecuteResponse,
                                           request.request_id, DEJAVU_CONTROL_RPC_OK, "42"};
    require(dejavu_control_write_rpc(fds[1], reply), "queue subsequent response");
    require(dejavu_control_relay_rpc(&agent, request, &response, Clock::now() + 80ms) &&
                response.status == DEJAVU_CONTROL_RPC_OK && response.payload == "42",
            "subsequent connection works");
    DejavuControlRpcMessage invalid_reconnect = {DejavuControlRpcType::kReconnectRequest, 18,
                                                 DEJAVU_CONTROL_RPC_OK, "invalid"};
    require(dejavu_control_relay_rpc(&agent, invalid_reconnect, &response, Clock::now() + 80ms) &&
                agent >= 0 && response.type == DejavuControlRpcType::kReconnectResponse &&
                response.status == DEJAVU_CONTROL_RPC_PROTOCOL_ERROR,
            "invalid reconnect preserves connection");
    invalid_reconnect.payload.clear();
    require(dejavu_control_relay_rpc(&agent, invalid_reconnect, &response, Clock::now() + 80ms) &&
                agent == -1 && response.type == DejavuControlRpcType::kReconnectResponse &&
                response.status == DEJAVU_CONTROL_RPC_OK,
            "reconnect clears connection");
    close(fds[1]);

    pair(fds);
    agent = fds[0];
    require(dejavu_control_relay_rpc(&agent, request, &response, Clock::now() + 80ms) &&
                agent == -1 && response.payload == kUnknown,
            "stalled reply bounded and unknown");
    close(fds[1]);
    pair(fds);
    agent = fds[0];
    auto mismatch = reply;
    ++mismatch.request_id;
    require(dejavu_control_write_rpc(fds[1], mismatch), "queue mismatched reply");
    require(dejavu_control_relay_rpc(&agent, request, &response, Clock::now() + 80ms) &&
                agent == -1 && response.payload == kUnknown,
            "mismatched reply is unknown");
    close(fds[1]);

    pair(fds);
    agent = fds[0];
    int small_buffer = 1024;
    require(setsockopt(agent, SOL_SOCKET, SO_SNDBUF, &small_buffer, sizeof(small_buffer)) == 0,
            "relay small buffer");
    auto large = request;
    large.payload.assign(kDejavuMaximumControlScriptSize, 'x');
    require(dejavu_control_relay_rpc(&agent, large, &response, Clock::now() + 80ms) &&
                agent == -1 && response.payload == kUnknown,
            "relay stalled dispatch bounded");
    close(fds[1]);
}
} // namespace

int main() {
    bounded(deadlines);
    bounded(listener_connect);
    bounded(partial_frame);
    bounded(interrupted_deadline);
    bounded(nested_rpc_deadline);
    bounded(malformed_and_eof);
    bounded(relay);
    std::cout << "OK: absolute control deadlines, peer credentials, and relay outcomes\n";
}
