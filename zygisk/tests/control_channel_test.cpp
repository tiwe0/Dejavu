#include "control_channel.h"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

[[noreturn]] void fail(const char *message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

}  // namespace

int main() {
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        fail("socketpair");
    }
    const std::string sent = "hook.log('live')";
    if (!dejavu_control_write_frame(sockets[0], sent.data(), sent.size())) {
        fail("write frame");
    }
    std::string received;
    if (!dejavu_control_read_frame(sockets[1], &received, 1024) || received != sent) {
        fail("read frame");
    }
    close(sockets[0]);
    close(sockets[1]);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0 ||
        !dejavu_control_write_client_info(sockets[0], 1234, "bin.mt.plus")) {
        fail("write client info");
    }
    DejavuControlClientInfo info;
    if (!dejavu_control_read_client_info(sockets[1], &info) || info.pid != 1234 ||
        info.process_name != "bin.mt.plus") {
        fail("read client info");
    }
    close(sockets[0]);
    close(sockets[1]);

    DejavuControlEndpoint endpoint = {};
    std::strcpy(endpoint.socket_name, "dejavu-test-socket");
    for (size_t index = 0; index < sizeof(endpoint.token); ++index) {
        endpoint.token[index] = static_cast<uint8_t>(index + 1);
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0 ||
        !dejavu_control_write_endpoint(sockets[0], endpoint)) {
        fail("write endpoint");
    }
    DejavuControlEndpoint received_endpoint = {};
    if (!dejavu_control_read_endpoint(sockets[1], &received_endpoint) ||
        std::strcmp(received_endpoint.socket_name, endpoint.socket_name) != 0 ||
        !std::equal(
            std::begin(received_endpoint.token),
            std::end(received_endpoint.token),
            std::begin(endpoint.token))) {
        fail("read endpoint");
    }
    close(sockets[0]);
    close(sockets[1]);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0 ||
        !dejavu_control_write_authentication(sockets[0], endpoint) ||
        !dejavu_control_read_authentication(sockets[1], endpoint)) {
        fail("accept valid token");
    }
    close(sockets[0]);
    close(sockets[1]);

    DejavuControlEndpoint wrong_endpoint = endpoint;
    wrong_endpoint.token[0] ^= 0xff;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0 ||
        !dejavu_control_write_authentication(sockets[0], wrong_endpoint) ||
        dejavu_control_read_authentication(sockets[1], endpoint)) {
        fail("reject invalid token");
    }
    close(sockets[0]);
    close(sockets[1]);

    std::string path;
    if (!dejavu_control_file_path("bin.mt.plus:worker", &path) ||
        path != "/data/adb/modules/dejavu_zygisk/run/bin.mt.plus:worker.lua") {
        fail("valid control path");
    }
    if (dejavu_control_file_path("../escape", &path) ||
        dejavu_control_file_path("bad/name", &path)) {
        fail("invalid control path accepted");
    }
    std::cout << "OK: authenticated abstract-socket control protocol\n";
    return 0;
}
