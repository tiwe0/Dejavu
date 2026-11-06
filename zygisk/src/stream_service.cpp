#include "stream_service.h"

#include "ikcp.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

namespace {

constexpr int kConnectTimeoutMs = 3000;
constexpr int kWriteTimeoutMs = 100;
constexpr size_t kMaximumUdpPayload = 65000;
constexpr size_t kMaximumStreamRecord = 32u * 1024u * 1024u;

std::atomic<unsigned int> g_next_conv{0x4B435001u};

uint32_t iclock_value() {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

bool split_endpoint(const std::string &endpoint, std::string *host, uint16_t *port,
                    std::string *error) {
    std::string host_part;
    std::string port_part;
    if (!endpoint.empty() && endpoint.front() == '[') {
        const size_t end = endpoint.find(']');
        if (end == std::string::npos || end + 1 >= endpoint.size() || endpoint[end + 1] != ':') {
            *error = "stream endpoint must be host:port";
            return false;
        }
        host_part = endpoint.substr(1, end - 1);
        port_part = endpoint.substr(end + 2);
    } else {
        const size_t colon = endpoint.rfind(':');
        if (colon == std::string::npos) {
            *error = "stream endpoint must be host:port";
            return false;
        }
        host_part = endpoint.substr(0, colon);
        port_part = endpoint.substr(colon + 1);
    }
    if (host_part.empty() || port_part.empty()) {
        *error = "stream endpoint must be host:port";
        return false;
    }
    try {
        const unsigned long value = std::stoul(port_part);
        if (value == 0 || value > 65535) {
            *error = "stream endpoint port is out of range";
            return false;
        }
        *port = static_cast<uint16_t>(value);
    } catch (const std::exception &) {
        *error = "stream endpoint port is invalid";
        return false;
    }
    *host = host_part;
    return true;
}

bool parse_numeric_address(const std::string &host, sockaddr_storage *storage,
                            socklen_t *length);

int connect_endpoint(uint32_t transport, const std::string &endpoint, std::string *error) {
    std::string host;
    uint16_t port = 0;
    if (!split_endpoint(endpoint, &host, &port, error)) {
        return -1;
    }
    const bool tcp = transport == DEJAVU_STREAM_TRANSPORT_TCP;
    sockaddr_storage numeric = {};
    socklen_t numeric_length = 0;
    if (parse_numeric_address(host, &numeric, &numeric_length)) {
        if (numeric.ss_family == AF_INET) {
            reinterpret_cast<sockaddr_in *>(&numeric)->sin_port = htons(port);
        } else {
            reinterpret_cast<sockaddr_in6 *>(&numeric)->sin6_port = htons(port);
        }
        const int fd = socket(numeric.ss_family, tcp ? SOCK_STREAM : SOCK_DGRAM,
                              tcp ? IPPROTO_TCP : IPPROTO_UDP);
        if (fd < 0) {
            *error = "unable to create stream socket";
            return -1;
        }
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        if (tcp) {
            const int original_flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, original_flags | O_NONBLOCK);
            if (connect(fd, reinterpret_cast<sockaddr *>(&numeric), numeric_length) != 0) {
                pollfd descriptor = {fd, POLLOUT, 0};
                const int ready = poll(&descriptor, 1, kConnectTimeoutMs);
                int socket_error = 0;
                socklen_t error_size = sizeof(socket_error);
                if (ready <= 0 ||
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) != 0 ||
                    socket_error != 0) {
                    close(fd);
                    *error = "unable to connect stream endpoint " + endpoint;
                    return -1;
                }
            }
            const int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        } else if (connect(fd, reinterpret_cast<sockaddr *>(&numeric), numeric_length) != 0) {
            close(fd);
            *error = "unable to connect stream endpoint " + endpoint;
            return -1;
        }
        return fd;
    }
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = tcp ? SOCK_STREAM : SOCK_DGRAM;
    hints.ai_protocol = tcp ? IPPROTO_TCP : IPPROTO_UDP;
    addrinfo *addresses = nullptr;
    const std::string port_text = std::to_string(port);
    const int lookup = getaddrinfo(host.c_str(), port_text.c_str(), &hints, &addresses);
    if (lookup != 0 || addresses == nullptr) {
        *error = std::string("unable to resolve stream endpoint: ") + gai_strerror(lookup);
        return -1;
    }
    int fd = -1;
    for (addrinfo *address = addresses; address != nullptr; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) {
            continue;
        }
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        if (tcp) {
            const int original_flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, original_flags | O_NONBLOCK);
            const int status = connect(fd, address->ai_addr, address->ai_addrlen);
            if (status != 0) {
                pollfd descriptor = {fd, POLLOUT, 0};
                const int ready = poll(&descriptor, 1, kConnectTimeoutMs);
                int socket_error = 0;
                socklen_t error_size = sizeof(socket_error);
                if (ready <= 0 ||
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) != 0 ||
                    socket_error != 0) {
                    close(fd);
                    fd = -1;
                    continue;
                }
            }
            const int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        } else {
            if (connect(fd, address->ai_addr, address->ai_addrlen) != 0) {
                close(fd);
                fd = -1;
                continue;
            }
        }
        break;
    }
    freeaddrinfo(addresses);
    if (fd < 0) {
        *error = "unable to connect stream endpoint " + endpoint;
    }
    return fd;
}

bool parse_numeric_address(const std::string &host, sockaddr_storage *storage,
                            socklen_t *length) {
    sockaddr_in ipv4 = {};
    if (inet_pton(AF_INET, host.c_str(), &ipv4.sin_addr) == 1) {
        ipv4.sin_family = AF_INET;
        std::memcpy(storage, &ipv4, sizeof(ipv4));
        *length = sizeof(ipv4);
        return true;
    }
    sockaddr_in6 ipv6 = {};
    if (inet_pton(AF_INET6, host.c_str(), &ipv6.sin6_addr) == 1) {
        ipv6.sin6_family = AF_INET6;
        std::memcpy(storage, &ipv6, sizeof(ipv6));
        *length = sizeof(ipv6);
        return true;
    }
    return false;
}

int create_listener(const std::string &endpoint, int backlog, std::string *error) {
    std::string host;
    uint16_t port = 0;
    if (!split_endpoint(endpoint, &host, &port, error)) {
        return -1;
    }
    sockaddr_storage numeric = {};
    socklen_t numeric_length = 0;
    const bool numeric_host = parse_numeric_address(host, &numeric, &numeric_length);
    auto bind_listen = [&](int fd, const sockaddr *address, socklen_t length) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        const int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, address, length) != 0 || ::listen(fd, backlog <= 0 ? 4 : backlog) != 0) {
            return false;
        }
        const int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        return true;
    };
    if (numeric_host) {
        if (numeric.ss_family == AF_INET) {
            reinterpret_cast<sockaddr_in *>(&numeric)->sin_port = htons(port);
        } else {
            reinterpret_cast<sockaddr_in6 *>(&numeric)->sin6_port = htons(port);
        }
        const int fd = socket(numeric.ss_family, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) {
            *error = "unable to create listen socket";
            return -1;
        }
        if (!bind_listen(fd, reinterpret_cast<sockaddr *>(&numeric), numeric_length)) {
            ::close(fd);
            *error = "unable to listen on " + endpoint;
            return -1;
        }
        return fd;
    }
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;
    addrinfo *addresses = nullptr;
    const std::string port_text = std::to_string(port);
    const int lookup = getaddrinfo(host.c_str(), port_text.c_str(), &hints, &addresses);
    if (lookup != 0 || addresses == nullptr) {
        *error = std::string("unable to resolve listen endpoint: ") + gai_strerror(lookup);
        return -1;
    }
    int fd = -1;
    for (addrinfo *address = addresses; address != nullptr; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (!bind_listen(fd, address->ai_addr, address->ai_addrlen)) {
            ::close(fd);
            fd = -1;
            continue;
        }
        break;
    }
    freeaddrinfo(addresses);
    if (fd < 0) {
        *error = "unable to listen on " + endpoint;
    }
    return fd;
}

bool build_frame(const void *data, size_t size, std::vector<char> *frame) {
    if (size > UINT32_MAX) {
        return false;
    }
    frame->resize(size + 4);
    const uint32_t length = htonl(static_cast<uint32_t>(size));
    std::memcpy(frame->data(), &length, sizeof(length));
    if (size != 0) {
        std::memcpy(frame->data() + 4, data, size);
    }
    return true;
}

bool send_all(int fd, const void *data, size_t size, const std::atomic<bool> &active,
              std::chrono::steady_clock::time_point deadline) {
    const char *bytes = static_cast<const char *>(data);
    size_t offset = 0;
    while (offset < size) {
        if (!active.load() || std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        const ssize_t written =
            send(fd, bytes + offset, size - offset, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (written > 0) {
            offset += static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            return false;
        }
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) {
            return false;
        }
        pollfd descriptor = {fd, POLLOUT, 0};
        const int timeout =
            static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count()) +
            1;
        const int ready = poll(&descriptor, 1, timeout);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return false;
        }
    }
    return true;
}

} // namespace

struct StreamService::Stream {
    uint32_t transport = 0;
    int fd = -1;
    uint32_t conv = 0;
    ikcpcb *kcp = nullptr;
    bool listener = false;
    std::timed_mutex mutex;
    std::timed_mutex read_mutex;
    int cancel_read_fd = -1;
    int cancel_write_fd = -1;
    std::string read_buffer;
    std::atomic<bool> active{true};
    std::thread worker;

    ~Stream() {
        active.store(false);
        if (worker.joinable()) worker.join();
        if (kcp != nullptr) ikcp_release(kcp);
        if (fd >= 0) ::close(fd);
        if (cancel_read_fd >= 0) ::close(cancel_read_fd);
        if (cancel_write_fd >= 0) ::close(cancel_write_fd);
    }
};

namespace {

using StreamClock = std::chrono::steady_clock;

bool prepare_cancellation(StreamService::Stream *stream, std::string *error) {
    int descriptors[2];
    if (pipe(descriptors) != 0) {
        *error = "unable to create stream cancellation pipe";
        return false;
    }
    stream->cancel_read_fd = descriptors[0];
    stream->cancel_write_fd = descriptors[1];
    for (int fd : descriptors) {
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0 ||
            fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
            *error = "unable to configure stream cancellation pipe";
            return false;
        }
    }
    return true;
}

void cancel_stream(StreamService::Stream *stream) {
    stream->active.store(false);
    if (stream->cancel_write_fd >= 0) {
        const char signal = 1;
        // Never drain this pipe: one signal wakes every concurrent poller.
        ssize_t result;
        do {
            result = ::write(stream->cancel_write_fd, &signal, sizeof(signal));
        } while (result < 0 && errno == EINTR);
    }
    if (stream->transport == DEJAVU_STREAM_TRANSPORT_TCP) {
        shutdown(stream->fd, SHUT_RDWR);
    }
}

void close_stream(const std::shared_ptr<StreamService::Stream> &stream) {
    cancel_stream(stream.get());
    if (stream->worker.joinable()) stream->worker.join();
    // Wait for current I/O without closing/reusing its fd. Remaining queued
    // operations retain the Stream and fail on active=false before using it.
    std::scoped_lock lock(stream->mutex, stream->read_mutex);
    stream->read_buffer.clear();
    if (stream->kcp != nullptr) {
        ikcp_flush(stream->kcp);
        ikcp_release(stream->kcp);
        stream->kcp = nullptr;
    }
}

bool lock_reader(StreamService::Stream *stream, int timeout_ms,
                 StreamClock::time_point deadline, std::unique_lock<std::timed_mutex> *lock) {
    if (!stream->active.load()) return false;
    if (timeout_ms == 0) return lock->try_lock();
    if (timeout_ms > 0) return lock->try_lock_until(deadline);
    while (stream->active.load()) {
        if (lock->try_lock_for(std::chrono::milliseconds(10))) return true;
    }
    return false;
}

// Return 1 for readable data, 0 for a deadline, and -1 for cancellation/error.
int wait_readable(StreamService::Stream *stream, int timeout_ms,
                  StreamClock::time_point deadline) {
    for (;;) {
        if (!stream->active.load()) return -1;
        int wait_ms = -1;
        if (timeout_ms == 0) {
            wait_ms = 0;
        } else if (timeout_ms > 0) {
            const auto remaining = deadline - StreamClock::now();
            if (remaining <= StreamClock::duration::zero()) return 0;
            wait_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count()) + 1;
        }
        pollfd descriptors[2] = {{stream->fd, POLLIN, 0},
                                 {stream->cancel_read_fd, POLLIN, 0}};
        const int ready = poll(descriptors, 2, wait_ms);
        if (ready < 0 && errno == EINTR) continue;
        if (!stream->active.load() || descriptors[1].revents != 0) return -1;
        if (ready == 0) return 0;
        if (ready < 0 || (descriptors[0].revents & (POLLERR | POLLNVAL)) != 0) return -1;
        return 1; // POLLHUP must reach recv(), which returns EOF after buffered data.
    }
}

int kcp_output(const char *buffer, int length, ikcpcb *, void *user) {
    auto *stream = static_cast<StreamService::Stream *>(user);
    if (stream == nullptr || stream->fd < 0) {
        return -1;
    }
    const ssize_t written = send(stream->fd, buffer, static_cast<size_t>(length), MSG_NOSIGNAL);
    return written == length ? 0 : -1;
}

void run_kcp_worker(StreamService::Stream *stream) {
    while (stream->active.load()) {
        pollfd descriptor = {stream->fd, POLLIN, 0};
        const int ready = poll(&descriptor, 1, 5);
        if (ready > 0 && (descriptor.revents & POLLIN) != 0) {
            char buffer[2048];
            while (true) {
                const ssize_t count = recv(stream->fd, buffer, sizeof(buffer), MSG_DONTWAIT);
                if (count <= 0) {
                    break;
                }
                std::lock_guard<std::timed_mutex> lock(stream->mutex);
                ikcp_input(stream->kcp, buffer, static_cast<long>(count));
            }
        }
        {
            std::lock_guard<std::timed_mutex> lock(stream->mutex);
            if (stream->kcp != nullptr) {
                ikcp_update(stream->kcp, iclock_value());
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

} // namespace

StreamService::StreamService() = default;

StreamService::~StreamService() {
    std::vector<int> handles;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &entry : streams_) {
            handles.push_back(entry.first);
        }
    }
    for (int handle : handles) {
        close(handle);
    }
}

int StreamService::open(uint64_t owner_token, uint32_t transport, const std::string &endpoint,
                        std::string *error) {
    auto stream = std::make_shared<Stream>();
    stream->transport = transport;
    if (transport == DEJAVU_STREAM_TRANSPORT_KCP) {
        if (open_kcp(stream.get(), endpoint, error) != 0) {
            return 0;
        }
    } else if (transport == DEJAVU_STREAM_TRANSPORT_TCP ||
               transport == DEJAVU_STREAM_TRANSPORT_UDP) {
        stream->fd = connect_endpoint(transport, endpoint, error);
        if (stream->fd < 0) {
            return 0;
        }
    } else {
        *error = "unsupported stream transport";
        return 0;
    }
    if (transport == DEJAVU_STREAM_TRANSPORT_TCP &&
        !prepare_cancellation(stream.get(), error)) return 0;
    return register_stream(stream, owner_token, error);
}

int StreamService::open_kcp(Stream *stream, const std::string &endpoint, std::string *error) {
    stream->fd = connect_endpoint(DEJAVU_STREAM_TRANSPORT_KCP, endpoint, error);
    if (stream->fd < 0) {
        return -1;
    }
    stream->conv = g_next_conv.fetch_add(1);
    stream->kcp = ikcp_create(stream->conv, stream);
    if (stream->kcp == nullptr) {
        ::close(stream->fd);
        stream->fd = -1;
        *error = "unable to allocate KCP state";
        return -1;
    }
    ikcp_setoutput(stream->kcp, kcp_output);
    ikcp_nodelay(stream->kcp, 1, 10, 2, 1);
    ikcp_wndsize(stream->kcp, 256, 256);
    stream->worker = std::thread(run_kcp_worker, stream);
    return 0;
}

int StreamService::listen(uint64_t owner_token, uint32_t transport,
                           const std::string &endpoint, int backlog, std::string *error) {
    if (transport != DEJAVU_STREAM_TRANSPORT_TCP) {
        *error = "stream listen supports TCP only";
        return 0;
    }
    auto stream = std::make_shared<Stream>();
    stream->transport = transport;
    stream->listener = true;
    stream->fd = create_listener(endpoint, backlog, error);
    if (stream->fd < 0) {
        return 0;
    }
    if (transport == DEJAVU_STREAM_TRANSPORT_TCP &&
        !prepare_cancellation(stream.get(), error)) return 0;
    return register_stream(stream, owner_token, error);
}

int StreamService::accept(int listener_handle, int timeout_ms, uint64_t expected_owner) {
    const auto deadline = StreamClock::now() + std::chrono::milliseconds(
        timeout_ms < 0 ? 0 : timeout_ms);
    auto listener = find(listener_handle, expected_owner);
    if (listener == nullptr || !listener->listener) return -1;
    std::unique_lock<std::timed_mutex> reader(listener->read_mutex, std::defer_lock);
    if (!lock_reader(listener.get(), timeout_ms, deadline, &reader))
        return listener->active.load() ? 0 : -1;
    int fd = -1;
    for (;;) {
        const int ready = wait_readable(listener.get(), timeout_ms, deadline);
        if (ready <= 0) return ready;
        fd = ::accept(listener->fd, nullptr, nullptr);
        if (fd >= 0) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
        return -1;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        ::close(fd);
        return -1;
    }
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    auto stream = std::make_shared<Stream>();
    stream->transport = DEJAVU_STREAM_TRANSPORT_TCP;
    stream->fd = fd;
    std::string error;
    if (!prepare_cancellation(stream.get(), &error)) return -1;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto entry = streams_.find(listener_handle);
    const auto owner = owners_.find(listener_handle);
    // Owner cleanup and accepted-connection publication use the same lock.
    if (entry == streams_.end() || entry->second != listener ||
        owner == owners_.end() || !listener->active.load() ||
        retired_owners_.count(owner->second) != 0) return -1;
    if (next_handle_ == std::numeric_limits<int>::max()) return -1;
    const int handle = next_handle_++;
    streams_[handle] = stream;
    owners_[handle] = owner->second;
    return handle;
}

int StreamService::read(int handle, void *buffer, size_t capacity, int timeout_ms,
                        size_t *size_read, uint64_t expected_owner) {
    const auto deadline = StreamClock::now() + std::chrono::milliseconds(
        timeout_ms < 0 ? 0 : timeout_ms);
    if (size_read != nullptr) *size_read = 0;
    if (buffer == nullptr && capacity != 0) return -1;
    auto stream = find(handle, expected_owner);
    if (stream == nullptr || stream->listener ||
        stream->transport != DEJAVU_STREAM_TRANSPORT_TCP) return -1;
    std::unique_lock<std::timed_mutex> read_lock(stream->read_mutex, std::defer_lock);
    if (!lock_reader(stream.get(), timeout_ms, deadline, &read_lock))
        return stream->active.load() ? 0 : -1;
    for (;;) {
        if (!stream->active.load()) return -1;
        if (stream->read_buffer.size() >= 4) {
            uint32_t network_length = 0;
            std::memcpy(&network_length, stream->read_buffer.data(), sizeof(network_length));
            const size_t length = ntohl(network_length);
            if (length > kMaximumStreamRecord || length > capacity) return -1;
            if (stream->read_buffer.size() >= 4 + length) {
                if (length != 0) std::memcpy(buffer, stream->read_buffer.data() + 4, length);
                stream->read_buffer.erase(0, 4 + length);
                if (size_read != nullptr) *size_read = length;
                return 1;
            }
        }
        const int ready = wait_readable(stream.get(), timeout_ms, deadline);
        if (ready <= 0) return ready;
        char chunk[65536];
        const ssize_t count = recv(stream->fd, chunk, sizeof(chunk), MSG_DONTWAIT);
        if (count > 0) {
            stream->read_buffer.append(chunk, static_cast<size_t>(count));
            continue;
        }
        if (count == 0) return -1;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
        return -1;
    }
}

std::shared_ptr<StreamService::Stream> StreamService::find(int handle, uint64_t expected_owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = streams_.find(handle);
    const auto owner = owners_.find(handle);
    if (iterator == streams_.end() || owner == owners_.end() ||
        (expected_owner != 0 && owner->second != expected_owner)) return nullptr;
    return iterator->second;
}

int StreamService::register_stream(const std::shared_ptr<Stream> &stream,
                                   uint64_t owner_token, std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (retired_owners_.count(owner_token) != 0) {
        *error = "stream owner is retired";
        return 0;
    }
    if (next_handle_ == std::numeric_limits<int>::max()) {
        *error = "stream handle space is exhausted";
        return 0;
    }
    const int handle = next_handle_++;
    streams_[handle] = stream;
    owners_[handle] = owner_token;
    return handle;
}

int StreamService::write(int handle, const void *data, size_t size, uint64_t expected_owner) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kWriteTimeoutMs);
    std::shared_ptr<Stream> stream = find(handle, expected_owner);
    if (stream == nullptr || stream->listener) {
        return -1;
    }
    std::vector<char> frame;
    if (!build_frame(data, size, &frame)) {
        return -1;
    }
    std::unique_lock<std::timed_mutex> lock(stream->mutex, std::defer_lock);
    if (stream->transport == DEJAVU_STREAM_TRANSPORT_TCP) {
        if (!lock.try_lock_until(deadline)) {
            return -1;
        }
    } else {
        lock.lock();
    }
    if (!stream->active.load() || stream->fd < 0) {
        return -1;
    }
    if (stream->transport == DEJAVU_STREAM_TRANSPORT_KCP) {
        if (stream->kcp == nullptr) {
            return -1;
        }
        if (ikcp_send(stream->kcp, frame.data(), static_cast<int>(frame.size())) < 0) {
            return -1;
        }
        ikcp_flush(stream->kcp);
        return 0;
    }
    if (stream->transport == DEJAVU_STREAM_TRANSPORT_UDP) {
        if (frame.size() > kMaximumUdpPayload) {
            return -1;
        }
        return send(stream->fd, frame.data(), frame.size(), MSG_NOSIGNAL) ==
                       static_cast<ssize_t>(frame.size())
                   ? 0
                   : -1;
    }
    if (send_all(stream->fd, frame.data(), frame.size(), stream->active, deadline)) {
        return 0;
    }
    // The peer may have received only part of this frame. Retire the stream so
    // a later write cannot corrupt its length-prefixed record sequence.
    cancel_stream(stream.get());
    return -1;
}

int StreamService::close(int handle, uint64_t expected_owner) {
    std::shared_ptr<Stream> stream;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto iterator = streams_.find(handle);
        const auto owner = owners_.find(handle);
        if (iterator == streams_.end() || owner == owners_.end() ||
            (expected_owner != 0 && owner->second != expected_owner)) return -1;
        stream = iterator->second;
        streams_.erase(iterator);
        owners_.erase(owner);
    }
    close_stream(stream);
    return 0;
}

bool StreamService::owned_by(int handle, uint64_t owner_token) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto owner = owners_.find(handle);
    return owner != owners_.end() && owner->second == owner_token;
}

size_t StreamService::close_owner(uint64_t owner_token) {
    std::vector<std::shared_ptr<Stream>> streams;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto owner = owners_.begin(); owner != owners_.end();) {
            if (owner->second != owner_token) {
                ++owner;
                continue;
            }
            const auto entry = streams_.find(owner->first);
            streams.push_back(entry->second);
            streams_.erase(entry);
            owner = owners_.erase(owner);
        }
    }
    // Cancel every stream before waiting for any operation to finish.
    for (const auto &stream : streams) cancel_stream(stream.get());
    for (const auto &stream : streams) close_stream(stream);
    return streams.size();
}

size_t StreamService::retire_owner(uint64_t owner_token) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        retired_owners_.insert(owner_token);
    }
    return close_owner(owner_token);
}
