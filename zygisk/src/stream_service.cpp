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
#include <thread>
#include <vector>

namespace {

constexpr int kConnectTimeoutMs = 3000;
constexpr int kWriteTimeoutMs = 100;
constexpr size_t kMaximumUdpPayload = 65000;

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

int connect_endpoint(uint32_t transport, const std::string &endpoint, std::string *error) {
    std::string host;
    uint16_t port = 0;
    if (!split_endpoint(endpoint, &host, &port, error)) {
        return -1;
    }
    const bool tcp = transport == DEJAVU_STREAM_TRANSPORT_TCP;
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
    std::timed_mutex mutex;
    std::atomic<bool> active{true};
    std::thread worker;
};

namespace {

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
    std::lock_guard<std::mutex> lock(mutex_);
    if (next_handle_ <= 0) {
        next_handle_ = 1;
    }
    const int handle = next_handle_++;
    streams_[handle] = stream;
    owners_[handle] = owner_token;
    return handle;
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

std::shared_ptr<StreamService::Stream> StreamService::find(int handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = streams_.find(handle);
    return iterator == streams_.end() ? nullptr : iterator->second;
}

int StreamService::write(int handle, const void *data, size_t size) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kWriteTimeoutMs);
    std::shared_ptr<Stream> stream = find(handle);
    if (stream == nullptr) {
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
    stream->active.store(false);
    shutdown(stream->fd, SHUT_RDWR);
    return -1;
}

int StreamService::close(int handle) {
    std::shared_ptr<Stream> stream;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto iterator = streams_.find(handle);
        if (iterator == streams_.end()) {
            return -1;
        }
        stream = iterator->second;
        streams_.erase(iterator);
        owners_.erase(handle);
    }
    stream->active.store(false);
    // The fd stays owned by this close operation until after the writer has
    // released the mutex. shutdown wakes poll/send without risking fd reuse.
    if (stream->transport == DEJAVU_STREAM_TRANSPORT_TCP) {
        shutdown(stream->fd, SHUT_RDWR);
    }
    if (stream->worker.joinable()) {
        stream->worker.join();
    }
    {
        std::lock_guard<std::timed_mutex> lock(stream->mutex);
        if (stream->kcp != nullptr) {
            ikcp_flush(stream->kcp);
            ikcp_release(stream->kcp);
            stream->kcp = nullptr;
        }
        if (stream->fd >= 0) {
            ::close(stream->fd);
            stream->fd = -1;
        }
    }
    return 0;
}

size_t StreamService::close_owner(uint64_t owner_token) {
    std::vector<int> handles;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &entry : owners_) {
            if (entry.second == owner_token) {
                handles.push_back(entry.first);
            }
        }
    }
    size_t closed = 0;
    for (int handle : handles) {
        if (close(handle) == 0) {
            ++closed;
        }
    }
    return closed;
}
