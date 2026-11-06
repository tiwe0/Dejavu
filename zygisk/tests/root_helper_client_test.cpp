// Exercise the production client without a device, real sockets or page changes.
#include "root_helper_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

int root_client_test_mprotect(void *, size_t, int);
int root_client_test_socket(int, int, int);
int root_client_test_connect(int, const sockaddr *, socklen_t);
ssize_t root_client_test_write(int, const void *, size_t);
ssize_t root_client_test_read(int, void *, size_t);
int root_client_test_close(int);

// Only intercept the client's explicit system calls; its selection, protocol,
// saved-range restoration and rollback implementations remain unchanged.
#define mprotect root_client_test_mprotect
#define socket root_client_test_socket
#define connect root_client_test_connect
#define write root_client_test_write
#define read root_client_test_read
#define close root_client_test_close
#include "root_helper_client.cpp"
#undef mprotect
#undef socket
#undef connect
#undef write
#undef read
#undef close

namespace {

struct Connection {
    bool connected = false;
    bool closed = false;
    std::string written;
    std::string response = "OK\nOK\n";
    size_t response_offset = 0;
};

struct Scenario {
    bool local_denied = false;
    bool helper_configured = true;
    bool helper_connectable = true;
    unsigned int property_reads = 0;
    std::vector<std::tuple<uintptr_t, size_t, int>> local_requests;
    std::vector<Connection> connections;
} scenario;

constexpr int kFirstFakeSocket = 20000;
constexpr uintptr_t kSavedStart = 0x5000;
constexpr size_t kSavedLength = 0x1000;

RootHelperProtectionSnapshot snapshot(int protection) {
    return {{{kSavedStart, kSavedLength, protection}}};
}

Connection &connection(int fd) {
    assert(fd >= kFirstFakeSocket);
    const auto index = static_cast<size_t>(fd - kFirstFakeSocket);
    assert(index < scenario.connections.size());
    return scenario.connections[index];
}

void expect_helper_request(size_t index, int protection) {
    const auto &entry = scenario.connections[index];
    assert(entry.connected && entry.closed);
    const size_t newline = entry.written.find('\n');
    assert(newline != std::string::npos);
    assert(entry.written.substr(0, newline) == std::string(64, 'a'));
    std::istringstream request(entry.written.substr(newline + 1));
    char command = 0;
    int pid = 0;
    unsigned long long start = 0, length = 0;
    int requested_protection = 0;
    request >> command >> pid >> std::hex >> start >> length >> std::dec >> requested_protection;
    assert(request && command == 'M' && pid == getpid());
    assert(start == kSavedStart && length == kSavedLength && requested_protection == protection);
}

void test_local_pages_do_not_depend_on_helper_connectivity(int original_protection) {
    scenario = {};
    scenario.helper_connectable = false;
    const auto saved = snapshot(original_protection);
    std::string error;
    assert(root_helper_apply_read_write(saved, &error) && error.empty());
    assert(root_helper_restore(saved, &error) && error.empty());
    assert(scenario.property_reads == 0 && scenario.connections.empty());
    assert(scenario.local_requests.size() == 2);
    assert(
        scenario.local_requests[0] ==
        std::make_tuple(kSavedStart, kSavedLength, original_protection | PROT_READ | PROT_WRITE));
    assert(scenario.local_requests[1] ==
           std::make_tuple(kSavedStart, kSavedLength, original_protection));
}

void test_local_denial_falls_back_to_helper(int original_protection) {
    scenario = {};
    scenario.local_denied = true;
    const auto saved = snapshot(original_protection);
    std::string error;
    assert(root_helper_apply_read_write(saved, &error) && error.empty());
    assert(root_helper_restore(saved, &error) && error.empty());
    assert(scenario.local_requests.size() == 2 && scenario.property_reads > 0);
    assert(scenario.connections.size() == 2);
    expect_helper_request(0, original_protection | PROT_READ | PROT_WRITE);
    expect_helper_request(1, original_protection);
}

void test_denied_local_without_helper_reports_failure(int original_protection) {
    scenario = {};
    scenario.local_denied = true;
    scenario.helper_configured = false;
    const auto saved = snapshot(original_protection);
    std::string error;
    assert(!root_helper_apply_read_write(saved, &error) && !error.empty());
    // The uncertain failed apply is rolled back using the same local-first policy.
    assert(scenario.local_requests.size() == 2 && scenario.connections.empty());
    assert(std::get<2>(scenario.local_requests[1]) == original_protection);
    assert(!root_helper_restore(saved, &error) && !error.empty());
    assert(scenario.local_requests.size() == 3 && scenario.connections.empty());
    assert(saved.ranges[0].start == kSavedStart && saved.ranges[0].length == kSavedLength);
}

void test_helper_connection_failure_closes_socket_and_keeps_snapshot() {
    scenario = {};
    scenario.local_denied = true;
    scenario.helper_connectable = false;
    const auto saved = snapshot(PROT_EXEC);
    std::string error;
    assert(!root_helper_apply_read_write(saved, &error));
    assert(error.find("unable to connect to root helper") != std::string::npos);
    assert(scenario.local_requests.size() == 2 && scenario.connections.size() == 2);
    for (const auto &entry : scenario.connections)
        assert(entry.closed && !entry.connected);
    // A later retry may succeed locally, even while stale helper properties remain.
    scenario.local_denied = false;
    assert(root_helper_restore(saved, &error) && error.empty());
    assert(scenario.local_requests.size() == 3 && scenario.connections.size() == 2);
}

} // namespace

extern "C" int __system_property_get(const char *name, char *value) {
    ++scenario.property_reads;
    value[0] = '\0';
    if (!scenario.helper_configured)
        return 0;
    const std::string text =
        std::strcmp(name, "debug.dejavu.root.port") == 0 ? "39401" : std::string(64, 'a');
    std::memcpy(value, text.c_str(), text.size() + 1);
    return static_cast<int>(text.size());
}

int root_client_test_mprotect(void *address, size_t size, int protection) {
    scenario.local_requests.emplace_back(reinterpret_cast<uintptr_t>(address), size, protection);
    if (!scenario.local_denied)
        return 0;
    errno = EACCES;
    return -1;
}

int root_client_test_socket(int domain, int type, int protocol) {
    assert(domain == AF_INET && type == SOCK_STREAM && protocol == 0);
    scenario.connections.emplace_back();
    return kFirstFakeSocket + static_cast<int>(scenario.connections.size()) - 1;
}

int root_client_test_connect(int fd, const sockaddr *address, socklen_t length) {
    assert(length == sizeof(sockaddr_in));
    const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(address);
    assert(ipv4->sin_family == AF_INET && ntohs(ipv4->sin_port) == 39401);
    assert(ipv4->sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    if (!scenario.helper_connectable) {
        errno = ECONNREFUSED;
        return -1;
    }
    connection(fd).connected = true;
    return 0;
}

ssize_t root_client_test_write(int fd, const void *data, size_t size) {
    auto &entry = connection(fd);
    assert(entry.connected && !entry.closed);
    entry.written.append(static_cast<const char *>(data), size);
    return static_cast<ssize_t>(size);
}

ssize_t root_client_test_read(int fd, void *data, size_t size) {
    auto &entry = connection(fd);
    assert(entry.connected && !entry.closed);
    const size_t available = entry.response.size() - entry.response_offset;
    const size_t copied = std::min(size, available);
    std::memcpy(data, entry.response.data() + entry.response_offset, copied);
    entry.response_offset += copied;
    return static_cast<ssize_t>(copied);
}

int root_client_test_close(int fd) {
    auto &entry = connection(fd);
    assert(!entry.closed);
    entry.closed = true;
    return 0;
}

int main() {
    for (int protection : {PROT_READ | PROT_EXEC, PROT_EXEC}) {
        test_local_pages_do_not_depend_on_helper_connectivity(protection);
        test_local_denial_falls_back_to_helper(protection);
        test_denied_local_without_helper_reports_failure(protection);
    }
    test_helper_connection_failure_closes_socket_and_keeps_snapshot();
    std::puts(
        "OK: real root helper client uses local-first RX/XOM apply and restore, with IPC fallback");
}
