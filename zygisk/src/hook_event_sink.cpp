#include "hook_event_sink.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>
#include <vector>

namespace {

constexpr size_t kMaxFrames = 2048;
constexpr size_t kMaxQueuedBytes = 8u * 1024u * 1024u;
constexpr int kConnectTimeoutMs = 3000;
#ifndef DEJAVU_EVENT_SEND_TIMEOUT_MS
#define DEJAVU_EVENT_SEND_TIMEOUT_MS 100
#endif
constexpr int kSendTimeoutMs = DEJAVU_EVENT_SEND_TIMEOUT_MS;
constexpr int kCancellationPollMs = 25;
using Clock = std::chrono::steady_clock;

bool parse_endpoint(const std::string &endpoint, sockaddr_in *address, std::string *error) {
    const size_t colon = endpoint.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= endpoint.size()) {
        *error = "stream endpoint must be host:port";
        return false;
    }
    const std::string host = endpoint.substr(0, colon);
    const std::string port_text = endpoint.substr(colon + 1);
    int port = 0;
    for (const char ch : port_text) {
        if (ch < '0' || ch > '9') {
            *error = "stream endpoint port must be decimal";
            return false;
        }
        port = port * 10 + (ch - '0');
        if (port > 65535) {
            *error = "stream endpoint port is out of range";
            return false;
        }
    }
    if (port <= 0) {
        *error = "stream endpoint port is invalid";
        return false;
    }
    sockaddr_in parsed = {};
    parsed.sin_family = AF_INET;
    parsed.sin_port = htons(static_cast<uint16_t>(port));
    if (host == "localhost") {
        parsed.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else if (inet_pton(AF_INET, host.c_str(), &parsed.sin_addr) != 1) {
        *error = "stream endpoint host must be an IPv4 address or localhost";
        return false;
    }
    *address = parsed;
    return true;
}

bool wait_writable(int fd, const std::atomic<bool> &stopping, Clock::time_point deadline) {
    while (!stopping.load()) {
        const auto remaining = deadline - Clock::now();
        if (remaining <= Clock::duration::zero())
            return false;
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
        const int timeout =
            std::min(kCancellationPollMs, static_cast<int>(milliseconds.count()) + 1);
        pollfd descriptor{fd, POLLOUT, 0};
        const int ready = poll(&descriptor, 1, timeout);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            return false;
        if (ready > 0 && (descriptor.revents & POLLOUT) != 0)
            return !stopping.load();
    }
    return false;
}

bool connect_socket(int fd, const sockaddr_in &address, const std::atomic<bool> &stopping) {
    const int status = connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address));
    if (status == 0)
        return !stopping.load();
    if (errno != EINPROGRESS && errno != EINTR)
        return false;
    const auto deadline = Clock::now() + std::chrono::milliseconds(kConnectTimeoutMs);
    if (!wait_writable(fd, stopping, deadline))
        return false;
    int error = 0;
    socklen_t size = sizeof(error);
    return getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0;
}

bool write_all(int fd, const char *data, size_t size, const std::atomic<bool> &stopping,
               Clock::time_point deadline) {
    size_t offset = 0;
    while (offset < size && !stopping.load()) {
        if (Clock::now() >= deadline)
            return false;
        const ssize_t written = send(fd, data + offset, size - offset, MSG_NOSIGNAL);
        if (written > 0) {
            offset += static_cast<size_t>(written);
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!wait_writable(fd, stopping, deadline))
                return false;
        } else {
            return false;
        }
    }
    return offset == size && !stopping.load();
}

} // namespace

struct HookEventSink::State {
    std::mutex lifecycle_mutex;
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::string> frames;
    size_t queued_bytes = 0;
    bool running = false;
    uint64_t lease = 0;
    uint64_t next_lease = 1;
    std::atomic<bool> stop_requested{false};
    int fd = -1;
    sockaddr_in address = {};
    std::string endpoint;
    std::thread worker;
    unsigned long long emitted = 0;
    unsigned long long dropped = 0;
    unsigned long long sent = 0;
    unsigned long long send_failures = 0;
    unsigned long long connect_failures = 0;
};

namespace {

void run_worker(HookEventSink::State *state) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    bool connected = false;
    bool failed = false;
    if (fd >= 0) {
        const int flags = fcntl(fd, F_GETFL, 0);
        const bool configured = flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
                                fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            // Only the worker closes this fd. stop() can shutdown it while this
            // same lock guarantees the numeric descriptor has not been reused.
            state->fd = fd;
        }
        connected = configured && !state->stop_requested.load() &&
                    connect_socket(fd, state->address, state->stop_requested);
    }
    while (connected && !state->stop_requested.load()) {
        std::string frame;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->ready.wait(
                lock, [state] { return state->stop_requested.load() || !state->frames.empty(); });
            if (state->stop_requested.load())
                break;
            frame = std::move(state->frames.front());
            state->frames.pop_front();
            state->queued_bytes -= frame.size();
        }
        const uint32_t length = htonl(static_cast<uint32_t>(frame.size()));
        const auto deadline = Clock::now() + std::chrono::milliseconds(kSendTimeoutMs);
        if (!write_all(fd, reinterpret_cast<const char *>(&length), sizeof(length),
                       state->stop_requested, deadline) ||
            !write_all(fd, frame.data(), frame.size(), state->stop_requested, deadline)) {
            failed = true;
            break;
        }
        std::lock_guard<std::mutex> lock(state->mutex);
        state->sent += 1;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    state->fd = -1;
    if (fd >= 0)
        ::close(fd);
    state->running = false;
    if (!state->stop_requested.load()) {
        if (!connected)
            state->connect_failures += 1;
        else if (failed)
            state->send_failures += 1;
    }
    state->queued_bytes = 0;
    state->frames.clear();
}

// Caller owns lifecycle_mutex, so worker creation, cancellation and join cannot
// overlap with another start/stop operation.
void stop_worker(HookEventSink::State *state) {
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->stop_requested.store(true);
        state->running = false;
        state->lease = 0;
        state->frames.clear();
        state->queued_bytes = 0;
        if (state->fd >= 0)
            shutdown(state->fd, SHUT_RDWR);
    }
    state->ready.notify_all();
    if (state->worker.joinable())
        state->worker.join();
    state->stop_requested.store(false);
}

bool start_worker(HookEventSink::State *state, const sockaddr_in &address,
                  const std::string &endpoint, std::string *error) {
    stop_worker(state);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->address = address;
        state->endpoint = endpoint;
        state->running = true;
    }
    try {
        state->worker = std::thread(run_worker, state);
    } catch (const std::exception &failure) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->running = false;
        state->frames.clear();
        state->queued_bytes = 0;
        *error = std::string("unable to start event worker: ") + failure.what();
        return false;
    }
    return true;
}

} // namespace

HookEventSink &HookEventSink::instance() {
    static HookEventSink sink;
    return sink;
}

HookEventSink::HookEventSink() : state_(new State()) {
}

HookEventSink::~HookEventSink() {
    stop();
    delete state_;
}

bool HookEventSink::start(const std::string &endpoint, std::string *error) {
    sockaddr_in address = {};
    if (!parse_endpoint(endpoint, &address, error)) {
        return false;
    }
    std::lock_guard<std::mutex> lifecycle_lock(state_->lifecycle_mutex);
    return start_worker(state_, address, endpoint, error);
}

void HookEventSink::stop() {
    std::lock_guard<std::mutex> lifecycle_lock(state_->lifecycle_mutex);
    stop_worker(state_);
}

bool HookEventSink::acquire(const std::string &endpoint, uint64_t *lease, std::string *error) {
    if (lease == nullptr) {
        *error = "missing event channel lease output";
        return false;
    }
    *lease = 0;
    sockaddr_in address{};
    if (!parse_endpoint(endpoint, &address, error))
        return false;
    std::lock_guard<std::mutex> lifecycle_lock(state_->lifecycle_mutex);
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->running || state_->lease != 0) {
            *error = "event channel is already owned; stop its owner before acquiring";
            return false;
        }
        if (state_->next_lease > static_cast<uint64_t>(std::numeric_limits<long long>::max())) {
            *error = "event channel lease space exhausted";
            return false;
        }
    }
    if (!start_worker(state_, address, endpoint, error))
        return false;
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->lease = state_->next_lease++;
    *lease = state_->lease;
    return true;
}

bool HookEventSink::release(uint64_t lease) {
    if (lease == 0)
        return false;
    std::lock_guard<std::mutex> lifecycle_lock(state_->lifecycle_mutex);
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->lease != lease)
            return false;
    }
    stop_worker(state_);
    return true;
}

bool HookEventSink::active() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->running;
}

bool HookEventSink::emit(const char *topic, const char *data, size_t size) {
    if (topic == nullptr || (data == nullptr && size != 0)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->running) {
            state_->dropped += 1;
            return false;
        }
    }
    const size_t topic_length = std::strlen(topic);
    if (topic_length == 0 || topic_length > 0xffff || size > kMaximumPayloadBytes) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->dropped += 1;
        return false;
    }
    std::string frame;
    frame.resize(2 + topic_length + size);
    frame[0] = static_cast<char>((topic_length >> 8) & 0xff);
    frame[1] = static_cast<char>(topic_length & 0xff);
    std::memcpy(&frame[2], topic, topic_length);
    if (size != 0) {
        std::memcpy(&frame[2 + topic_length], data, size);
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->running || state_->frames.size() >= kMaxFrames ||
        state_->queued_bytes + frame.size() > kMaxQueuedBytes) {
        state_->dropped += 1;
        return false;
    }
    state_->queued_bytes += frame.size();
    state_->frames.push_back(std::move(frame));
    state_->emitted += 1;
    state_->ready.notify_one();
    return true;
}

void HookEventSink::record_drop() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->dropped += 1;
}

std::string HookEventSink::stats() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    std::ostringstream output;
    output << "active=" << (state_->running ? 1 : 0) << " endpoint=" << state_->endpoint
           << " queued=" << state_->frames.size() << " emitted=" << state_->emitted
           << " dropped=" << state_->dropped << " sent=" << state_->sent
           << " send_failures=" << state_->send_failures
           << " connect_failures=" << state_->connect_failures;
    return output.str();
}
