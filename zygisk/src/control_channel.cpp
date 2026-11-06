#include "control_channel.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <limits>
#include <utility>

namespace {

constexpr char kControlDirectory[] = "/data/adb/modules/dejavu_zygisk/run/";
constexpr uint32_t kClientInfoMagic = 0x444A4349;
constexpr uint32_t kEndpointMagic = 0x444A4550;
constexpr uint32_t kRpcMagic = 0x444A5250;
constexpr uint32_t kControlProtocolVersion = 1;
constexpr char kSocketNamePrefix[] = "dejavu-";
constexpr size_t kRpcHeaderSize = 28;

struct ClientInfoHeader {
    uint32_t magic;
    uint32_t version;
    int32_t pid;
    uint32_t process_name_size;
};

struct EndpointHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t socket_name_size;
    uint32_t token_size;
};

static_assert(sizeof(EndpointHeader) == sizeof(uint32_t) * 4);

bool deadline_is_live(DejavuControlDeadline deadline) {
    if (deadline != DejavuControlDeadline::max() && std::chrono::steady_clock::now() >= deadline) {
        errno = ETIMEDOUT;
        return false;
    }
    return true;
}

bool wait_ready(int fd, short events, DejavuControlDeadline deadline) {
    for (;;) {
        if (!deadline_is_live(deadline)) {
            return false;
        }
        int timeout = -1;
        if (deadline != DejavuControlDeadline::max()) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            // Round up so a fractional millisecond does not expire early.
            const auto milliseconds =
                std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
            timeout = milliseconds > std::numeric_limits<int>::max()
                          ? std::numeric_limits<int>::max()
                          : static_cast<int>(milliseconds);
            if (timeout <= 0) {
                errno = ETIMEDOUT;
                return false;
            }
        }
        pollfd descriptor = {fd, events, 0};
        const int result = poll(&descriptor, 1, timeout);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0) {
            return false;
        }
        if (result == 0) {
            continue;
        }
        if (descriptor.revents & POLLNVAL) {
            errno = EBADF;
            return false;
        }
        // HUP may accompany final buffered bytes. Let recv/send consume data
        // or report the real terminal error rather than dropping that data.
        return true;
    }
}

bool set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && ((flags & O_NONBLOCK) != 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

bool read_all(int fd, void *buffer, size_t size, DejavuControlDeadline deadline) {
    if (!set_nonblocking(fd))
        return false;
    auto *bytes = static_cast<unsigned char *>(buffer);
    size_t offset = 0;
    while (offset < size) {
        if (!deadline_is_live(deadline))
            return false;
        const ssize_t count = recv(fd, bytes + offset, size - offset, MSG_DONTWAIT);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!wait_ready(fd, POLLIN, deadline))
                return false;
            continue;
        }
        if (count <= 0)
            return false;
        offset += static_cast<size_t>(count);
    }
    return true;
}

bool write_all(int fd, const void *buffer, size_t size, DejavuControlDeadline deadline) {
    // Darwin can block large sends despite MSG_DONTWAIT on a blocking socket.
    if (!set_nonblocking(fd))
        return false;
    const auto *bytes = static_cast<const unsigned char *>(buffer);
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0)
        return false;
#endif
    size_t offset = 0;
    while (offset < size) {
        if (!deadline_is_live(deadline))
            return false;
        int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        const ssize_t count = send(fd, bytes + offset, size - offset, flags);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!wait_ready(fd, POLLOUT, deadline))
                return false;
            continue;
        }
        if (count <= 0)
            return false;
        offset += static_cast<size_t>(count);
    }
    return true;
}

bool is_process_character(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '.' || value == '_' || value == '-' ||
           value == ':';
}

bool is_socket_name_character(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '.' || value == '_' || value == '-';
}

bool endpoint_is_valid(const DejavuControlEndpoint &endpoint) {
    const size_t name_size = strnlen(endpoint.socket_name, kDejavuControlSocketNameCapacity);
    if (name_size < sizeof(kSocketNamePrefix) || name_size == kDejavuControlSocketNameCapacity ||
        strncmp(endpoint.socket_name, kSocketNamePrefix, sizeof(kSocketNamePrefix) - 1) != 0) {
        return false;
    }
    for (size_t index = 0; index < name_size; ++index) {
        if (!is_socket_name_character(endpoint.socket_name[index])) {
            return false;
        }
    }
    uint8_t token_bits = 0;
    for (uint8_t value : endpoint.token) {
        token_bits |= value;
    }
    return token_bits != 0;
}

bool read_random(void *buffer, size_t size) {
    const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    auto *bytes = static_cast<unsigned char *>(buffer);
    size_t offset = 0;
    while (offset < size) {
        const ssize_t count = read(fd, bytes + offset, size - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        offset += static_cast<size_t>(count);
    }
    const bool success = offset == size;
    close(fd);
    return success;
}

bool generate_endpoint(DejavuControlEndpoint *endpoint) {
    uint8_t random[16 + kDejavuControlTokenSize] = {};
    if (endpoint == nullptr || !read_random(random, sizeof(random))) {
        return false;
    }
    constexpr char hex[] = "0123456789abcdef";
    memcpy(endpoint->socket_name, kSocketNamePrefix, sizeof(kSocketNamePrefix) - 1);
    size_t offset = sizeof(kSocketNamePrefix) - 1;
    for (size_t index = 0; index < 16; ++index) {
        endpoint->socket_name[offset++] = hex[random[index] >> 4];
        endpoint->socket_name[offset++] = hex[random[index] & 0x0f];
    }
    endpoint->socket_name[offset] = '\0';
    memcpy(endpoint->token, random + 16, sizeof(endpoint->token));
    return endpoint_is_valid(*endpoint);
}

bool fill_abstract_address(const DejavuControlEndpoint &endpoint, sockaddr_un *address,
                           socklen_t *address_size) {
    if (address == nullptr || address_size == nullptr || !endpoint_is_valid(endpoint)) {
        return false;
    }
    const size_t name_size = strlen(endpoint.socket_name);
    if (name_size + 1 > sizeof(address->sun_path)) {
        return false;
    }
    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    memcpy(address->sun_path + 1, endpoint.socket_name, name_size);
    *address_size = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name_size);
    return true;
}

int open_unix_socket() {
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    const int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
        close(fd);
        return -1;
    }
    const int status_flags = fcntl(fd, F_GETFL);
    if (status_flags < 0 || fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) != 0) {
        const int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

bool constant_time_equal(const uint8_t *left, const uint8_t *right, size_t size) {
    uint8_t difference = 0;
    for (size_t index = 0; index < size; ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0;
}

void write_u32_le(char *output, uint32_t value) {
    for (size_t index = 0; index < sizeof(value); ++index) {
        output[index] = static_cast<char>((value >> (index * 8)) & 0xff);
    }
}

void write_u64_le(char *output, uint64_t value) {
    for (size_t index = 0; index < sizeof(value); ++index) {
        output[index] = static_cast<char>((value >> (index * 8)) & 0xff);
    }
}

uint32_t read_u32_le(const char *input) {
    uint32_t value = 0;
    for (size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<uint32_t>(static_cast<uint8_t>(input[index])) << (index * 8);
    }
    return value;
}

uint64_t read_u64_le(const char *input) {
    uint64_t value = 0;
    for (size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<uint64_t>(static_cast<uint8_t>(input[index])) << (index * 8);
    }
    return value;
}

bool rpc_type_is_valid(DejavuControlRpcType type) {
    return type == DejavuControlRpcType::kExecuteRequest ||
           type == DejavuControlRpcType::kExecuteResponse ||
           type == DejavuControlRpcType::kReconnectRequest ||
           type == DejavuControlRpcType::kReconnectResponse;
}

bool rpc_type_is_request(DejavuControlRpcType type) {
    return type == DejavuControlRpcType::kExecuteRequest ||
           type == DejavuControlRpcType::kReconnectRequest;
}

bool rpc_status_is_valid(int32_t status) {
    return status == DEJAVU_CONTROL_RPC_OK || status == DEJAVU_CONTROL_RPC_EXECUTION_ERROR ||
           status == DEJAVU_CONTROL_RPC_UNAVAILABLE || status == DEJAVU_CONTROL_RPC_PROTOCOL_ERROR;
}

} // namespace

bool dejavu_control_read_frame(int fd, std::string *payload, size_t maximum_size,
                               DejavuControlDeadline deadline) {
    uint32_t size = 0;
    if (fd < 0 || payload == nullptr || !read_all(fd, &size, sizeof(size), deadline) ||
        size > maximum_size) {
        return false;
    }
    payload->resize(size);
    return size == 0 || read_all(fd, payload->data(), size, deadline);
}

bool dejavu_control_write_frame(int fd, const void *payload, size_t payload_size,
                                DejavuControlDeadline deadline) {
    if (fd < 0 || payload_size > std::numeric_limits<uint32_t>::max() ||
        (payload == nullptr && payload_size != 0)) {
        return false;
    }
    const uint32_t size = static_cast<uint32_t>(payload_size);
    return write_all(fd, &size, sizeof(size), deadline) &&
           (size == 0 || write_all(fd, payload, size, deadline));
}

bool dejavu_control_file_path(const std::string &process_name, std::string *path) {
    if (path == nullptr || process_name.empty() || process_name.size() > 128) {
        return false;
    }
    for (char value : process_name) {
        if (!is_process_character(value)) {
            return false;
        }
    }
    *path = kControlDirectory + process_name + ".lua";
    return true;
}

bool dejavu_control_write_client_info(int fd, pid_t pid, const std::string &process_name,
                                      DejavuControlDeadline deadline) {
    std::string unused;
    if (pid <= 0 || !dejavu_control_file_path(process_name, &unused)) {
        return false;
    }
    const ClientInfoHeader header = {
        kClientInfoMagic,
        kControlProtocolVersion,
        static_cast<int32_t>(pid),
        static_cast<uint32_t>(process_name.size()),
    };
    std::string payload(sizeof(header) + process_name.size(), '\0');
    memcpy(payload.data(), &header, sizeof(header));
    memcpy(payload.data() + sizeof(header), process_name.data(), process_name.size());
    return dejavu_control_write_frame(fd, payload.data(), payload.size(), deadline);
}

bool dejavu_control_read_client_info(int fd, DejavuControlClientInfo *info,
                                     DejavuControlDeadline deadline) {
    std::string payload;
    if (info == nullptr ||
        !dejavu_control_read_frame(fd, &payload, sizeof(ClientInfoHeader) + 128, deadline) ||
        payload.size() < sizeof(ClientInfoHeader)) {
        return false;
    }
    ClientInfoHeader header = {};
    memcpy(&header, payload.data(), sizeof(header));
    if (header.magic != kClientInfoMagic || header.version != kControlProtocolVersion ||
        header.pid <= 0 || header.process_name_size == 0 || header.process_name_size > 128 ||
        payload.size() != sizeof(header) + header.process_name_size) {
        return false;
    }
    DejavuControlClientInfo parsed = {
        static_cast<pid_t>(header.pid),
        payload.substr(sizeof(header), header.process_name_size),
    };
    std::string unused;
    if (!dejavu_control_file_path(parsed.process_name, &unused)) {
        return false;
    }
    *info = std::move(parsed);
    return true;
}

bool dejavu_control_write_endpoint(int fd, const DejavuControlEndpoint &endpoint,
                                   DejavuControlDeadline deadline) {
    if (!endpoint_is_valid(endpoint)) {
        return false;
    }
    const uint32_t name_size = static_cast<uint32_t>(strlen(endpoint.socket_name));
    const EndpointHeader header = {
        kEndpointMagic,
        kControlProtocolVersion,
        name_size,
        kDejavuControlTokenSize,
    };
    std::string payload(sizeof(header) + name_size + kDejavuControlTokenSize, '\0');
    memcpy(payload.data(), &header, sizeof(header));
    memcpy(payload.data() + sizeof(header), endpoint.socket_name, name_size);
    memcpy(payload.data() + sizeof(header) + name_size, endpoint.token, kDejavuControlTokenSize);
    return dejavu_control_write_frame(fd, payload.data(), payload.size(), deadline);
}

bool dejavu_control_read_endpoint(int fd, DejavuControlEndpoint *endpoint,
                                  DejavuControlDeadline deadline) {
    std::string payload;
    if (endpoint == nullptr ||
        !dejavu_control_read_frame(fd, &payload,
                                   sizeof(EndpointHeader) + kDejavuControlSocketNameCapacity - 1 +
                                       kDejavuControlTokenSize,
                                   deadline) ||
        payload.size() < sizeof(EndpointHeader)) {
        return false;
    }
    EndpointHeader header = {};
    memcpy(&header, payload.data(), sizeof(header));
    if (header.magic != kEndpointMagic || header.version != kControlProtocolVersion ||
        header.socket_name_size == 0 ||
        header.socket_name_size >= kDejavuControlSocketNameCapacity ||
        header.token_size != kDejavuControlTokenSize ||
        payload.size() != sizeof(header) + header.socket_name_size + header.token_size) {
        return false;
    }
    DejavuControlEndpoint parsed = {};
    memcpy(parsed.socket_name, payload.data() + sizeof(header), header.socket_name_size);
    memcpy(parsed.token, payload.data() + sizeof(header) + header.socket_name_size,
           header.token_size);
    if (!endpoint_is_valid(parsed)) {
        return false;
    }
    *endpoint = parsed;
    return true;
}

bool dejavu_control_write_authentication(int fd, const DejavuControlEndpoint &endpoint,
                                         DejavuControlDeadline deadline) {
    return endpoint_is_valid(endpoint) &&
           dejavu_control_write_frame(fd, endpoint.token, sizeof(endpoint.token), deadline);
}

bool dejavu_control_read_authentication(int fd, const DejavuControlEndpoint &endpoint,
                                        DejavuControlDeadline deadline) {
    std::string token;
    return endpoint_is_valid(endpoint) &&
           dejavu_control_read_frame(fd, &token, kDejavuControlTokenSize, deadline) &&
           token.size() == kDejavuControlTokenSize &&
           constant_time_equal(reinterpret_cast<const uint8_t *>(token.data()), endpoint.token,
                               kDejavuControlTokenSize);
}

bool dejavu_control_write_rpc(int fd, const DejavuControlRpcMessage &message,
                              DejavuControlDeadline deadline) {
    if (!rpc_type_is_valid(message.type) || message.request_id == 0 ||
        !rpc_status_is_valid(message.status) ||
        message.payload.size() > kDejavuMaximumControlScriptSize ||
        (rpc_type_is_request(message.type) && message.status != DEJAVU_CONTROL_RPC_OK)) {
        return false;
    }
    std::string payload(kRpcHeaderSize + message.payload.size(), '\0');
    write_u32_le(payload.data(), kRpcMagic);
    write_u32_le(payload.data() + 4, kControlProtocolVersion);
    write_u32_le(payload.data() + 8, static_cast<uint32_t>(message.type));
    write_u32_le(payload.data() + 12, static_cast<uint32_t>(message.status));
    write_u64_le(payload.data() + 16, message.request_id);
    write_u32_le(payload.data() + 24, static_cast<uint32_t>(message.payload.size()));
    if (!message.payload.empty()) {
        memcpy(payload.data() + kRpcHeaderSize, message.payload.data(), message.payload.size());
    }
    return dejavu_control_write_frame(fd, payload.data(), payload.size(), deadline);
}

bool dejavu_control_read_rpc(int fd, DejavuControlRpcMessage *message,
                             DejavuControlDeadline deadline) {
    std::string payload;
    if (message == nullptr ||
        !dejavu_control_read_frame(fd, &payload, kRpcHeaderSize + kDejavuMaximumControlScriptSize,
                                   deadline) ||
        payload.size() < kRpcHeaderSize || read_u32_le(payload.data()) != kRpcMagic ||
        read_u32_le(payload.data() + 4) != kControlProtocolVersion) {
        return false;
    }
    const auto type = static_cast<DejavuControlRpcType>(read_u32_le(payload.data() + 8));
    const int32_t status = static_cast<int32_t>(read_u32_le(payload.data() + 12));
    const uint64_t request_id = read_u64_le(payload.data() + 16);
    const uint32_t payload_size = read_u32_le(payload.data() + 24);
    if (!rpc_type_is_valid(type) || request_id == 0 || !rpc_status_is_valid(status) ||
        payload_size > kDejavuMaximumControlScriptSize ||
        payload.size() != kRpcHeaderSize + payload_size ||
        (rpc_type_is_request(type) && status != DEJAVU_CONTROL_RPC_OK)) {
        return false;
    }
    *message = {
        type,
        request_id,
        status,
        payload.substr(kRpcHeaderSize, payload_size),
    };
    return true;
}

int dejavu_control_open_listener(DejavuControlEndpoint *endpoint) {
    if (endpoint == nullptr) {
        return -1;
    }
    for (int attempt = 0; attempt < 4; ++attempt) {
        *endpoint = {};
        sockaddr_un address = {};
        socklen_t address_size = 0;
        if (!generate_endpoint(endpoint) ||
            !fill_abstract_address(*endpoint, &address, &address_size)) {
            return -1;
        }
        const int fd = open_unix_socket();
        if (fd < 0) {
            return -1;
        }
        if (bind(fd, reinterpret_cast<const sockaddr *>(&address), address_size) == 0 &&
            listen(fd, 1) == 0) {
            return fd;
        }
        close(fd);
    }
    *endpoint = {};
    return -1;
}

int dejavu_control_connect(const DejavuControlEndpoint &endpoint, DejavuControlDeadline deadline) {
    sockaddr_un address = {};
    socklen_t address_size = 0;
    if (!deadline_is_live(deadline) || !fill_abstract_address(endpoint, &address, &address_size)) {
        return -1;
    }
    const int fd = open_unix_socket();
    if (fd < 0)
        return -1;
    bool connected = connect(fd, reinterpret_cast<const sockaddr *>(&address), address_size) == 0;
    if (!connected && (errno == EINPROGRESS || errno == EINTR)) {
        if (wait_ready(fd, POLLOUT, deadline)) {
            int socket_error = 0;
            socklen_t error_size = sizeof(socket_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) == 0) {
                connected = socket_error == 0;
                if (!connected)
                    errno = socket_error;
            }
        }
    }
    if (!connected) {
        const int connect_error = errno;
        close(fd);
        errno = connect_error;
        return -1;
    }
    return fd;
}

bool dejavu_control_peer_is_root(int fd) {
#if defined(__linux__)
    struct ucred credentials = {};
    socklen_t size = sizeof(credentials);
    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0 &&
           size == sizeof(credentials) && credentials.uid == 0;
#elif defined(__APPLE__)
    uid_t uid = static_cast<uid_t>(-1);
    gid_t gid = static_cast<gid_t>(-1);
    return getpeereid(fd, &uid, &gid) == 0 && uid == 0;
#else
    (void)fd;
    return false;
#endif
}

bool dejavu_control_relay_rpc(int *agent_fd, const DejavuControlRpcMessage &request,
                              DejavuControlRpcMessage *response, DejavuControlDeadline deadline) {
    if (agent_fd == nullptr || response == nullptr)
        return false;
    *response = {DejavuControlRpcType::kExecuteResponse, request.request_id,
                 DEJAVU_CONTROL_RPC_PROTOCOL_ERROR, "execute request required"};
    if (request.type == DejavuControlRpcType::kReconnectRequest) {
        response->type = DejavuControlRpcType::kReconnectResponse;
        if (!request.payload.empty()) {
            response->payload = "reconnect request payload must be empty";
            return true;
        }
        if (*agent_fd >= 0)
            close(*agent_fd);
        *agent_fd = -1;
        response->status = DEJAVU_CONTROL_RPC_OK;
        response->payload = "Agent reconnect requested";
        return true;
    }
    if (request.type != DejavuControlRpcType::kExecuteRequest)
        return true;
    response->status = DEJAVU_CONTROL_RPC_UNAVAILABLE;
    if (*agent_fd < 0) {
        response->payload = "target Agent is not connected";
        return true;
    }
    DejavuControlRpcMessage agent_response;
    if (!dejavu_control_write_rpc(*agent_fd, request, deadline) ||
        !dejavu_control_read_rpc(*agent_fd, &agent_response, deadline) ||
        agent_response.type != DejavuControlRpcType::kExecuteResponse ||
        agent_response.request_id != request.request_id) {
        close(*agent_fd);
        *agent_fd = -1;
        response->payload =
            "RPC execution outcome unknown; request may have executed; do not retry automatically";
        return true;
    }
    *response = std::move(agent_response);
    return true;
}
