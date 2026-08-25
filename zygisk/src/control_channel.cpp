#include "control_channel.h"

#include <errno.h>
#include <fcntl.h>
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
constexpr uint32_t kControlProtocolVersion = 1;
constexpr char kSocketNamePrefix[] = "dejavu-";

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

bool read_all(int fd, void *buffer, size_t size) {
    auto *bytes = static_cast<unsigned char *>(buffer);
    size_t offset = 0;
    while (offset < size) {
        const ssize_t count = read(fd, bytes + offset, size - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        offset += static_cast<size_t>(count);
    }
    return true;
}

bool write_all(int fd, const void *buffer, size_t size) {
    const auto *bytes = static_cast<const unsigned char *>(buffer);
    size_t offset = 0;
    while (offset < size) {
#ifdef MSG_NOSIGNAL
        const ssize_t count = send(fd, bytes + offset, size - offset, MSG_NOSIGNAL);
#else
        const ssize_t count = write(fd, bytes + offset, size - offset);
#endif
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
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
    const size_t name_size = strnlen(
        endpoint.socket_name, kDejavuControlSocketNameCapacity);
    if (name_size < sizeof(kSocketNamePrefix) ||
        name_size == kDejavuControlSocketNameCapacity ||
        strncmp(
            endpoint.socket_name,
            kSocketNamePrefix,
            sizeof(kSocketNamePrefix) - 1) != 0) {
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
    const bool success = read_all(fd, buffer, size);
    close(fd);
    return success;
}

bool generate_endpoint(DejavuControlEndpoint *endpoint) {
    uint8_t random[16 + kDejavuControlTokenSize] = {};
    if (endpoint == nullptr || !read_random(random, sizeof(random))) {
        return false;
    }
    constexpr char hex[] = "0123456789abcdef";
    memcpy(
        endpoint->socket_name,
        kSocketNamePrefix,
        sizeof(kSocketNamePrefix) - 1);
    size_t offset = sizeof(kSocketNamePrefix) - 1;
    for (size_t index = 0; index < 16; ++index) {
        endpoint->socket_name[offset++] = hex[random[index] >> 4];
        endpoint->socket_name[offset++] = hex[random[index] & 0x0f];
    }
    endpoint->socket_name[offset] = '\0';
    memcpy(endpoint->token, random + 16, sizeof(endpoint->token));
    return endpoint_is_valid(*endpoint);
}

bool fill_abstract_address(
    const DejavuControlEndpoint &endpoint,
    sockaddr_un *address,
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
    *address_size = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + 1 + name_size);
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
    return fd;
}

bool constant_time_equal(const uint8_t *left, const uint8_t *right, size_t size) {
    uint8_t difference = 0;
    for (size_t index = 0; index < size; ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0;
}

}  // namespace

bool dejavu_control_read_frame(int fd, std::string *payload, size_t maximum_size) {
    uint32_t size = 0;
    if (fd < 0 || payload == nullptr || !read_all(fd, &size, sizeof(size)) ||
        size > maximum_size) {
        return false;
    }
    payload->resize(size);
    return size == 0 || read_all(fd, payload->data(), size);
}

bool dejavu_control_write_frame(int fd, const void *payload, size_t payload_size) {
    if (fd < 0 || payload_size > std::numeric_limits<uint32_t>::max() ||
        (payload == nullptr && payload_size != 0)) {
        return false;
    }
    const uint32_t size = static_cast<uint32_t>(payload_size);
    return write_all(fd, &size, sizeof(size)) &&
        (size == 0 || write_all(fd, payload, size));
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

bool dejavu_control_write_client_info(
    int fd, pid_t pid, const std::string &process_name) {
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
    return dejavu_control_write_frame(fd, payload.data(), payload.size());
}

bool dejavu_control_read_client_info(int fd, DejavuControlClientInfo *info) {
    std::string payload;
    if (info == nullptr ||
        !dejavu_control_read_frame(fd, &payload, sizeof(ClientInfoHeader) + 128) ||
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

bool dejavu_control_write_endpoint(int fd, const DejavuControlEndpoint &endpoint) {
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
    std::string payload(
        sizeof(header) + name_size + kDejavuControlTokenSize, '\0');
    memcpy(payload.data(), &header, sizeof(header));
    memcpy(payload.data() + sizeof(header), endpoint.socket_name, name_size);
    memcpy(
        payload.data() + sizeof(header) + name_size,
        endpoint.token,
        kDejavuControlTokenSize);
    return dejavu_control_write_frame(fd, payload.data(), payload.size());
}

bool dejavu_control_read_endpoint(int fd, DejavuControlEndpoint *endpoint) {
    std::string payload;
    if (endpoint == nullptr ||
        !dejavu_control_read_frame(
            fd,
            &payload,
            sizeof(EndpointHeader) + kDejavuControlSocketNameCapacity - 1 +
                kDejavuControlTokenSize) ||
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
    memcpy(
        parsed.token,
        payload.data() + sizeof(header) + header.socket_name_size,
        header.token_size);
    if (!endpoint_is_valid(parsed)) {
        return false;
    }
    *endpoint = parsed;
    return true;
}

bool dejavu_control_write_authentication(
    int fd, const DejavuControlEndpoint &endpoint) {
    return endpoint_is_valid(endpoint) &&
        dejavu_control_write_frame(fd, endpoint.token, sizeof(endpoint.token));
}

bool dejavu_control_read_authentication(
    int fd, const DejavuControlEndpoint &endpoint) {
    std::string token;
    return endpoint_is_valid(endpoint) &&
        dejavu_control_read_frame(fd, &token, kDejavuControlTokenSize) &&
        token.size() == kDejavuControlTokenSize &&
        constant_time_equal(
            reinterpret_cast<const uint8_t *>(token.data()),
            endpoint.token,
            kDejavuControlTokenSize);
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

int dejavu_control_connect(const DejavuControlEndpoint &endpoint) {
    sockaddr_un address = {};
    socklen_t address_size = 0;
    if (!fill_abstract_address(endpoint, &address, &address_size)) {
        return -1;
    }
    const int fd = open_unix_socket();
    if (fd < 0) {
        return -1;
    }
    if (connect(fd, reinterpret_cast<const sockaddr *>(&address), address_size) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}
