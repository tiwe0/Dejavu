// Host-side live viewer for Dejavu framework streams.
//
// Usage: stream_receiver --transport tcp|udp|kcp --port N [--bind ADDR]
//
// Every stream.write() call becomes one length-prefixed record. TCP/UDP send
// the prefix directly; KCP carries the same length-prefixed records inside the
// KCP stream. Payloads are printed verbatim to stdout.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include "ikcp.h"
}

namespace {

struct KcpSession {
    sockaddr_storage peer = {};
    socklen_t peer_length = 0;
    ikcpcb *kcp = nullptr;
    std::string incoming;
    unsigned long long records = 0;
};

std::map<unsigned int, KcpSession> g_sessions;
int g_kcp_socket = -1;

void write_stdout(const void *data, size_t size) {
    const char *bytes = static_cast<const char *>(data);
    size_t offset = 0;
    while (offset < size) {
        const ssize_t written = write(STDOUT_FILENO, bytes + offset, size - offset);
        if (written <= 0) {
            return;
        }
        offset += static_cast<size_t>(written);
    }
    static const char newline = '\n';
    if (size == 0 || bytes[size - 1] != '\n') {
        (void)!write(STDOUT_FILENO, &newline, 1);
    }
}

void parse_frame(std::string *buffer) {
    while (buffer->size() >= 4) {
        uint32_t length = 0;
        std::memcpy(&length, buffer->data(), 4);
        length = ntohl(length);
        if (buffer->size() < 4 + static_cast<size_t>(length)) {
            return;
        }
        write_stdout(buffer->data() + 4, length);
        buffer->erase(0, 4 + static_cast<size_t>(length));
    }
}

int kcp_output(const char *buffer, int length, ikcpcb *, void *user) {
    auto *session = static_cast<KcpSession *>(user);
    if (g_kcp_socket < 0 || session == nullptr) {
        return -1;
    }
    const ssize_t sent =
        sendto(g_kcp_socket, buffer, static_cast<size_t>(length), 0,
               reinterpret_cast<const sockaddr *>(&session->peer), session->peer_length);
    return sent == length ? 0 : -1;
}

KcpSession *session_for(unsigned int conv, const sockaddr_storage &peer, socklen_t peer_length) {
    auto iterator = g_sessions.find(conv);
    if (iterator != g_sessions.end()) {
        iterator->second.peer = peer;
        iterator->second.peer_length = peer_length;
        return &iterator->second;
    }
    auto inserted = g_sessions.emplace(conv, KcpSession{});
    KcpSession &session = inserted.first->second;
    session.peer = peer;
    session.peer_length = peer_length;
    session.kcp = ikcp_create(conv, &session);
    if (session.kcp == nullptr) {
        g_sessions.erase(inserted.first);
        return nullptr;
    }
    ikcp_setoutput(session.kcp, kcp_output);
    ikcp_nodelay(session.kcp, 1, 10, 2, 1);
    ikcp_wndsize(session.kcp, 256, 256);
    return &session;
}

int run_tcp(int listen_fd) {
    while (true) {
        const int client = accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) {
            continue;
        }
        std::string buffer;
        char chunk[8192];
        while (true) {
            const ssize_t count = read(client, chunk, sizeof(chunk));
            if (count <= 0) {
                break;
            }
            buffer.append(chunk, static_cast<size_t>(count));
            parse_frame(&buffer);
        }
        close(client);
    }
    return 0;
}

int run_udp(int fd) {
    std::vector<char> buffer(65536);
    while (true) {
        const ssize_t count = recv(fd, buffer.data(), buffer.size(), 0);
        if (count < 4) {
            continue;
        }
        uint32_t length = 0;
        std::memcpy(&length, buffer.data(), 4);
        length = ntohl(length);
        if (static_cast<size_t>(count) == 4 + static_cast<size_t>(length)) {
            write_stdout(buffer.data() + 4, length);
        }
    }
    return 0;
}

void kcp_update_sessions() {
    for (auto &entry : g_sessions) {
        KcpSession &session = entry.second;
        if (session.kcp == nullptr) {
            continue;
        }
        ikcp_update(session.kcp,
                    static_cast<IUINT32>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now().time_since_epoch())
                                             .count()));
        char buffer[8192];
        while (true) {
            const int count = ikcp_recv(session.kcp, buffer, sizeof(buffer));
            if (count <= 0) {
                break;
            }
            session.incoming.append(buffer, static_cast<size_t>(count));
            parse_frame(&session.incoming);
        }
    }
}

int run_kcp(int fd) {
    g_kcp_socket = fd;
    std::vector<char> buffer(65536);
    while (true) {
        pollfd descriptor = {fd, POLLIN, 0};
        const int ready = poll(&descriptor, 1, 5);
        if (ready > 0 && (descriptor.revents & POLLIN) != 0) {
            while (true) {
                sockaddr_storage peer = {};
                socklen_t peer_length = sizeof(peer);
                const ssize_t count = recvfrom(fd, buffer.data(), buffer.size(), MSG_DONTWAIT,
                                               reinterpret_cast<sockaddr *>(&peer), &peer_length);
                if (count < 4) {
                    break;
                }
                unsigned int conv = 0;
                std::memcpy(&conv, buffer.data(), 4);
                KcpSession *session = session_for(conv, peer, peer_length);
                if (session != nullptr) {
                    ikcp_input(session->kcp, buffer.data(), static_cast<long>(count));
                }
            }
        }
        kcp_update_sessions();
    }
    return 0;
}

int make_socket(const char *transport, const char *bind_address, int port) {
    const bool tcp = std::strcmp(transport, "tcp") == 0;
    const int fd = socket(AF_INET, tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (fd < 0) {
        std::perror("socket");
        return -1;
    }
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (bind_address == nullptr || *bind_address == '\0') {
        address.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, bind_address, &address.sin_addr) != 1) {
        std::fprintf(stderr, "invalid bind address: %s\n", bind_address);
        close(fd);
        return -1;
    }
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        std::perror("bind");
        close(fd);
        return -1;
    }
    if (tcp && listen(fd, 8) != 0) {
        std::perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

} // namespace

int main(int argc, char **argv) {
    const char *transport = "tcp";
    const char *bind_address = "0.0.0.0";
    int port = 39001;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--transport") == 0 && index + 1 < argc) {
            transport = argv[++index];
        } else if (std::strcmp(argv[index], "--bind") == 0 && index + 1 < argc) {
            bind_address = argv[++index];
        } else if (std::strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
            port = std::atoi(argv[++index]);
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[index]);
            return 2;
        }
    }
    if (std::strcmp(transport, "tcp") != 0 && std::strcmp(transport, "udp") != 0 &&
        std::strcmp(transport, "kcp") != 0) {
        std::fprintf(stderr, "transport must be tcp, udp or kcp\n");
        return 2;
    }
    if (port <= 0 || port > 65535) {
        std::fprintf(stderr, "port is out of range\n");
        return 2;
    }
    const int fd = make_socket(transport, bind_address, port);
    if (fd < 0) {
        return 1;
    }
    setvbuf(stdout, nullptr, _IOLBF, 0);
    int status = 0;
    if (std::strcmp(transport, "tcp") == 0) {
        status = run_tcp(fd);
    } else if (std::strcmp(transport, "udp") == 0) {
        status = run_udp(fd);
    } else {
        status = run_kcp(fd);
    }
    close(fd);
    return status;
}
