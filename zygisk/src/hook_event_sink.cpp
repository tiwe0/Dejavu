#include "hook_event_sink.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace {

constexpr size_t kMaxFrames = 2048;
constexpr size_t kMaxQueuedBytes = 8u * 1024u * 1024u;
constexpr size_t kMaxFrameBytes = 4u * 1024u * 1024u;

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

bool write_all(int fd, const char *data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        const ssize_t written = send(fd, data + offset, size - offset, MSG_NOSIGNAL);
        if (written <= 0) {
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    return true;
}

}  // namespace

struct HookEventSink::State {
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::string> frames;
    size_t queued_bytes = 0;
    bool running = false;
    bool stop_requested = false;
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
    if (fd < 0) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->running = false;
        state->connect_failures += 1;
        return;
    }
    if (connect(fd, reinterpret_cast<const sockaddr *>(&state->address),
                sizeof(state->address)) != 0) {
        ::close(fd);
        std::lock_guard<std::mutex> lock(state->mutex);
        state->running = false;
        state->connect_failures += 1;
        return;
    }
    bool failed = false;
    while (!failed) {
        std::string frame;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->ready.wait(lock, [state] {
                return state->stop_requested || !state->frames.empty();
            });
            if (state->stop_requested) {
                break;
            }
            frame = std::move(state->frames.front());
            state->frames.pop_front();
            state->queued_bytes -= frame.size();
        }
        uint32_t length = htonl(static_cast<uint32_t>(frame.size()));
        if (!write_all(fd, reinterpret_cast<const char *>(&length), sizeof(length)) ||
            !write_all(fd, frame.data(), frame.size())) {
            failed = true;
            break;
        }
        std::lock_guard<std::mutex> lock(state->mutex);
        state->sent += 1;
    }
    ::close(fd);
    std::lock_guard<std::mutex> lock(state->mutex);
    state->running = false;
    if (failed) {
        state->send_failures += 1;
        state->queued_bytes = 0;
        state->frames.clear();
    }
}

}  // namespace

HookEventSink &HookEventSink::instance() {
    static HookEventSink sink;
    return sink;
}

HookEventSink::HookEventSink() : state_(new State()) {}

HookEventSink::~HookEventSink() {
    stop();
    delete state_;
}

bool HookEventSink::start(const std::string &endpoint, std::string *error) {
    sockaddr_in address = {};
    if (!parse_endpoint(endpoint, &address, error)) {
        return false;
    }
    stop();
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->address = address;
        state_->endpoint = endpoint;
        state_->stop_requested = false;
        state_->running = true;
    }
    state_->worker = std::thread(run_worker, state_);
    return true;
}

void HookEventSink::stop() {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->stop_requested = true;
        state_->running = false;
        state_->frames.clear();
        state_->queued_bytes = 0;
    }
    state_->ready.notify_all();
    if (state_->worker.joinable()) {
        state_->worker.join();
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->stop_requested = false;
}

bool HookEventSink::active() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->running;
}

bool HookEventSink::emit(const char *topic, const char *data, size_t size) {
    if (topic == nullptr) {
        return false;
    }
    const size_t topic_length = std::strlen(topic);
    if (topic_length == 0 || topic_length > 0xffff || size > kMaxFrameBytes) {
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
