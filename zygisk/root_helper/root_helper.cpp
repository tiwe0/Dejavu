// Prototype root helper for Dejavu.
// Runs as root (adb shell su / Magisk service) and injects mprotect into a
// target process via ptrace, so the in-app agent can Dobby-hook execute-only
// pages.
#include <elf.h>
#include <fcntl.h>
#include <signal.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr long kMprotectSyscall = 226; // __NR_mprotect, arm64

bool read_line(int fd, std::string *line) {
    line->clear();
    char ch = 0;
    while (true) {
        const ssize_t count = read(fd, &ch, 1);
        if (count <= 0) {
            return !line->empty();
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

bool write_all(int fd, const char *data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        const ssize_t written = write(fd, data + offset, size - offset);
        if (written <= 0) {
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool remote_syscall_gadget(pid_t pid, unsigned long long *gadget) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/maps", static_cast<int>(pid));
    FILE *maps = std::fopen(path, "re");
    if (maps == nullptr) {
        return false;
    }
    char line[512];
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long start = 0;
        unsigned long long end = 0;
        char permissions[8] = {};
        if (std::sscanf(line, "%llx-%llx %7s", &start, &end, permissions) != 3) {
            continue;
        }
        if (permissions[0] != 'r' || permissions[2] != 'x' || end - start < 4) {
            continue;
        }
        const size_t length = static_cast<size_t>((end - start) < 65536 ? (end - start) : 65536);
        std::vector<char> code(length);
        iovec local = {code.data(), code.size()};
        iovec remote = {reinterpret_cast<void *>(start), code.size()};
        if (process_vm_readv(pid, &local, 1, &remote, 1, 0) != static_cast<ssize_t>(code.size())) {
            continue;
        }
        for (size_t offset = 0; offset + 4 <= code.size(); offset += 4) {
            uint32_t instruction = 0;
            std::memcpy(&instruction, code.data() + offset, 4);
            if (instruction == 0xD4000001u) { // svc #0
                *gadget = start + offset;
                std::fclose(maps);
                return true;
            }
        }
    }
    std::fclose(maps);
    return false;
}

int remote_mprotect(pid_t pid, unsigned long long address, unsigned long long length,
                    int protection, std::string *error) {
    unsigned long long gadget = 0;
    if (!remote_syscall_gadget(pid, &gadget)) {
        *error = "unable to find an svc #0 gadget in target";
        return -1;
    }
    if (ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) != 0) {
        *error = std::string("ptrace attach failed: ") + std::strerror(errno);
        return -1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status)) {
        *error = "target did not stop for ptrace";
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return -1;
    }
    struct user_pt_regs saved = {};
    iovec registers = {&saved, sizeof(saved)};
    if (ptrace(PTRACE_GETREGSET, pid, reinterpret_cast<void *>(NT_PRSTATUS), &registers) != 0) {
        *error = std::string("PTRACE_GETREGSET failed: ") + std::strerror(errno);
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return -1;
    }
    struct user_pt_regs injected = saved;
    injected.regs[0] = address;
    injected.regs[1] = length;
    injected.regs[2] = static_cast<unsigned long>(protection);
    injected.regs[8] = kMprotectSyscall;
    injected.pc = gadget;
    injected.regs[30] = 0;
    iovec injected_registers = {&injected, sizeof(injected)};
    if (ptrace(PTRACE_SETREGSET, pid, reinterpret_cast<void *>(NT_PRSTATUS), &injected_registers) !=
        0) {
        *error = std::string("PTRACE_SETREGSET failed: ") + std::strerror(errno);
        ptrace(PTRACE_SETREGSET, pid, reinterpret_cast<void *>(NT_PRSTATUS), &registers);
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return -1;
    }
    if (ptrace(PTRACE_CONT, pid, nullptr, nullptr) != 0) {
        *error = std::string("PTRACE_CONT failed: ") + std::strerror(errno);
        ptrace(PTRACE_SETREGSET, pid, reinterpret_cast<void *>(NT_PRSTATUS), &registers);
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return -1;
    }
    if (waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status)) {
        *error = "target did not trap after mprotect";
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return -1;
    }
    struct user_pt_regs result = {};
    iovec result_registers = {&result, sizeof(result)};
    if (ptrace(PTRACE_GETREGSET, pid, reinterpret_cast<void *>(NT_PRSTATUS), &result_registers) !=
        0) {
        *error = "unable to read mprotect result";
        ptrace(PTRACE_SETREGSET, pid, reinterpret_cast<void *>(NT_PRSTATUS), &registers);
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return -1;
    }
    const long return_value = static_cast<long>(result.regs[0]);
    ptrace(PTRACE_SETREGSET, pid, reinterpret_cast<void *>(NT_PRSTATUS), &registers);
    ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
    if (return_value != 0) {
        *error = "target mprotect failed: errno=" + std::to_string(-return_value);
        return -1;
    }
    return 0;
}

int run_server(int port, const std::string &token) {
    signal(SIGPIPE, SIG_IGN);
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("socket");
        return 1;
    }
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        std::perror("bind");
        close(fd);
        return 1;
    }
    if (listen(fd, 8) != 0) {
        std::perror("listen");
        close(fd);
        return 1;
    }
    std::fprintf(stderr, "root helper listening on 127.0.0.1:%d\n", port);
    while (true) {
        const int client = accept(fd, nullptr, nullptr);
        if (client < 0) {
            continue;
        }
        std::string provided_token;
        if (!read_line(client, &provided_token) || provided_token != token) {
            write_all(client, "ERR token\n", 10);
            close(client);
            continue;
        }
        write_all(client, "OK\n", 3);
        while (true) {
            std::string request;
            if (!read_line(client, &request)) {
                break;
            }
            int pid = 0;
            unsigned long long address = 0;
            unsigned long long length = 0;
            int protection = 0;
            if (std::sscanf(request.c_str(), "M %d %llx %llx %d", &pid, &address, &length,
                            &protection) != 4) {
                write_all(client, "ERR request\n", 12);
                continue;
            }
            std::string error;
            if (remote_mprotect(pid, address, length, protection, &error) != 0) {
                std::string response = "ERR " + error + "\n";
                write_all(client, response.data(), response.size());
            } else {
                write_all(client, "OK\n", 3);
            }
        }
        close(client);
    }
    close(fd);
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    int port = 39401;
    std::string token;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
            port = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--token") == 0 && index + 1 < argc) {
            token = argv[++index];
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[index]);
            return 2;
        }
    }
    if (port <= 0 || port > 65535 || token.empty()) {
        std::fprintf(stderr, "usage: %s [--port 39401] --token TOKEN\n", argv[0]);
        return 2;
    }
    return run_server(port, token);
}
