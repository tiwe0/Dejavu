#include "root_helper_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace {

constexpr int kDefaultPort = 39401;
constexpr const char *kPortProperty = "debug.dejavu.root.port";
constexpr const char *kTokenProperty = "debug.dejavu.root.token";

bool write_line(int fd, const std::string &text) {
    const std::string line = text + "\n";
    size_t offset = 0;
    while (offset < line.size()) {
        const ssize_t written = write(fd, line.data() + offset, line.size() - offset);
        if (written <= 0) {
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool read_line(int fd, std::string *line) {
    line->clear();
    char ch = 0;
    while (true) {
        const ssize_t count = read(fd, &ch, 1);
        if (count <= 0) {
            return false;
        }
        if (ch == '\n') {
            return true;
        }
        line->push_back(ch);
        if (line->size() > 4096) {
            return false;
        }
    }
}

int connect_helper(int port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool send_mprotect(void *address, size_t length, int protection, std::string *error) {
    char port_text[PROP_VALUE_MAX] = {};
    char token[PROP_VALUE_MAX] = {};
    if (__system_property_get(kPortProperty, port_text) <= 0 ||
        __system_property_get(kTokenProperty, token) <= 0 || token[0] == '\0') {
        *error = "root helper endpoint/token properties are not set";
        return false;
    }
    int port = kDefaultPort;
    if (port_text[0] != '\0') {
        port = std::atoi(port_text);
        if (port <= 0 || port > 65535) {
            *error = "root helper port is invalid";
            return false;
        }
    }
    const int fd = connect_helper(port);
    if (fd < 0) {
        *error = "unable to connect to root helper";
        return false;
    }
    bool ok = false;
    std::string response;
    if (!write_line(fd, token) || !read_line(fd, &response) || response != "OK") {
        *error = response.empty() ? "root helper authentication failed" : response;
    } else {
        char request[128];
        std::snprintf(request, sizeof(request), "M %d %llx %llx %d", static_cast<int>(getpid()),
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address)),
                      static_cast<unsigned long long>(length), protection);
        if (!write_line(fd, request) || !read_line(fd, &response)) {
            *error = "root helper request failed";
        } else if (response == "OK") {
            ok = true;
        } else {
            *error = response;
        }
    }
    close(fd);
    return ok;
}

} // namespace

bool root_helper_available() {
    char port_text[PROP_VALUE_MAX] = {};
    char token[PROP_VALUE_MAX] = {};
    return __system_property_get(kPortProperty, port_text) > 0 &&
           __system_property_get(kTokenProperty, token) > 0 && token[0] != '\0';
}

bool root_helper_mapping(void *address, uintptr_t *start, size_t *length, int *protection,
                         std::string *error) {
    FILE *maps = std::fopen("/proc/self/maps", "re");
    if (maps == nullptr) {
        *error = "unable to open /proc/self/maps";
        return false;
    }
    const uintptr_t value = reinterpret_cast<uintptr_t>(address);
    char line[512];
    bool found = false;
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long map_start = 0;
        unsigned long map_end = 0;
        char permissions[8] = {};
        if (std::sscanf(line, "%lx-%lx %7s", &map_start, &map_end, permissions) != 3) {
            continue;
        }
        if (value < map_start || value >= map_end) {
            continue;
        }
        int prot = 0;
        if (permissions[0] == 'r') {
            prot |= PROT_READ;
        }
        if (permissions[1] == 'w') {
            prot |= PROT_WRITE;
        }
        if (permissions[2] == 'x') {
            prot |= PROT_EXEC;
        }
        *start = map_start;
        *length = map_end - map_start;
        *protection = prot;
        found = true;
        break;
    }
    std::fclose(maps);
    if (!found) {
        *error = "target address is not mapped";
        return false;
    }
    return true;
}

bool root_helper_capture(void *address, size_t size, RootHelperProtectionSnapshot *snapshot,
                         std::string *error) {
    snapshot->ranges.clear();
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        *error = "unable to determine the runtime page size";
        return false;
    }
    std::ifstream maps("/proc/self/maps");
    if (!maps.is_open()) {
        *error = "unable to open /proc/self/maps";
        return false;
    }
    return capture_mapping_protection(maps, reinterpret_cast<uintptr_t>(address), size,
                                      static_cast<size_t>(page_size), snapshot, error);
}

namespace {

bool apply_saved_range(const RootHelperProtectionRange &range, int protection, std::string *error) {
    // ART hooks are installed before app specialization, when helper IPC may
    // still be unavailable. Let the kernel decide whether the current process
    // may change the page, including XOM; use the helper only when it cannot.
    if (mprotect(reinterpret_cast<void *>(range.start), range.length, protection) == 0) {
        return true;
    }
    const int local_error = errno;
    if (!root_helper_available()) {
        *error = std::string("mprotect failed: ") + std::strerror(local_error);
        return false;
    }
    return send_mprotect(reinterpret_cast<void *>(range.start), range.length, protection, error);
}

} // namespace

bool root_helper_apply_read_write(const RootHelperProtectionSnapshot &snapshot,
                                  std::string *error) {
    return apply_mapping_read_write(snapshot, apply_saved_range, error);
}

bool root_helper_restore(const RootHelperProtectionSnapshot &snapshot, std::string *error) {
    return restore_mapping_protection(snapshot, apply_saved_range, error);
}
