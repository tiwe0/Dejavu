// Dejavu root helper.
//
// Runs as root (Magisk/KernelSU service) and injects mprotect into an
// authorized target process via ptrace. It only accepts mprotect requests for
// targets listed in config/targets.txt, and only for executable mappings.
#include <elf.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/system_properties.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr long kMprotectSyscall = 226; // __NR_mprotect, arm64
constexpr int kDefaultPort = 0;        // 0 = pick an ephemeral port
constexpr const char *kPortProperty = "debug.dejavu.root.port";
constexpr const char *kTokenProperty = "debug.dejavu.root.token";

int g_listen_fd = -1;
volatile sig_atomic_t g_stop = 0;

void clear_endpoint_properties() {
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    __system_property_set(kPortProperty, "");
    __system_property_set(kTokenProperty, "");
}

void handle_signal(int) {
    g_stop = 1;
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
}

std::string trim(std::string value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string random_token() {
    unsigned char bytes[32] = {};
    const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return {};
    }
    const ssize_t count = read(fd, bytes, sizeof(bytes));
    close(fd);
    if (count != static_cast<ssize_t>(sizeof(bytes))) {
        return {};
    }
    static const char digits[] = "0123456789abcdef";
    std::string token;
    token.reserve(sizeof(bytes) * 2);
    for (unsigned char byte : bytes) {
        token.push_back(digits[byte >> 4]);
        token.push_back(digits[byte & 0x0F]);
    }
    return token;
}

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

std::vector<std::string> load_targets(const std::string &path) {
    std::vector<std::string> targets;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (!line.empty() && line[0] != '#') {
            targets.push_back(line);
        }
    }
    return targets;
}

std::string process_name(pid_t pid) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/cmdline", static_cast<int>(pid));
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return {};
    }
    std::string name;
    char ch = 0;
    while (file.get(ch) && ch != '\0') {
        name.push_back(ch);
        if (name.size() > 256) {
            break;
        }
    }
    return name;
}

bool pid_allowed(pid_t pid, const std::string &targets_path) {
    const std::string name = process_name(pid);
    if (name.empty()) {
        return false;
    }
    const std::vector<std::string> targets = load_targets(targets_path);
    return std::find(targets.begin(), targets.end(), name) != targets.end();
}

bool target_mapping(pid_t pid, unsigned long long address, unsigned long long length,
                    std::string *error) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/maps", static_cast<int>(pid));
    FILE *maps = std::fopen(path, "re");
    if (maps == nullptr) {
        *error = "unable to read target maps";
        return false;
    }
    if (length == 0 || address + length < address) {
        *error = "invalid target range";
        std::fclose(maps);
        return false;
    }
    unsigned long long covered = address;
    char line[512];
    bool found = false;
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long start = 0;
        unsigned long long end = 0;
        char permissions[8] = {};
        if (std::sscanf(line, "%llx-%llx %7s", &start, &end, permissions) != 3) {
            continue;
        }
        if (end <= address || start >= address + length) {
            continue;
        }
        if (permissions[2] != 'x') {
            *error = "requested range contains a non-executable mapping";
            std::fclose(maps);
            return false;
        }
        if (start <= covered) {
            if (end > covered) {
                covered = end;
            }
            found = true;
        }
    }
    std::fclose(maps);
    if (!found || covered < address + length) {
        *error = "requested range is not fully executable in the target";
        return false;
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
            if (instruction == 0xD4000001u) {
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

bool valid_protection(int protection) {
    return (protection & PROT_EXEC) != 0 &&
           (protection & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) == 0;
}

void publish_endpoint(int port, const std::string &token) {
    char port_text[16];
    std::snprintf(port_text, sizeof(port_text), "%d", port);
    __system_property_set(kPortProperty, port_text);
    __system_property_set(kTokenProperty, token.c_str());
    std::fprintf(stderr, "root helper ready on 127.0.0.1:%d\n", port);
}

int run_server(int port, const std::string &token, const std::string &targets_path) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("socket");
        return 1;
    }
    g_listen_fd = fd;
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        std::perror("bind");
        close(fd);
        g_listen_fd = -1;
        return 1;
    }
    if (listen(fd, 8) != 0) {
        std::perror("listen");
        close(fd);
        g_listen_fd = -1;
        return 1;
    }
    socklen_t length = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
        std::perror("getsockname");
        close(fd);
        g_listen_fd = -1;
        return 1;
    }
    publish_endpoint(ntohs(address.sin_port), token);
    while (!g_stop) {
        const int client = accept(fd, nullptr, nullptr);
        if (client < 0) {
            if (g_stop) {
                break;
            }
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
            if (!pid_allowed(pid, targets_path)) {
                write_all(client, "ERR target is not allowlisted\n", 29);
                continue;
            }
            if (!valid_protection(protection)) {
                write_all(client, "ERR protection is not executable/readable\n", 40);
                continue;
            }
            std::string validation_error;
            if (!target_mapping(pid, address, length, &validation_error)) {
                std::string response = "ERR " + validation_error + "\n";
                write_all(client, response.data(), response.size());
                continue;
            }
            std::string helper_error;
            if (remote_mprotect(pid, address, length, protection, &helper_error) != 0) {
                std::string response = "ERR " + helper_error + "\n";
                write_all(client, response.data(), response.size());
            } else {
                write_all(client, "OK\n", 3);
            }
        }
        close(client);
    }
    clear_endpoint_properties();
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    int port = kDefaultPort;
    std::string token;
    std::string targets_path;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
            port = std::atoi(argv[++index]);
        } else if (std::strcmp(argv[index], "--token") == 0 && index + 1 < argc) {
            token = argv[++index];
        } else if (std::strcmp(argv[index], "--targets") == 0 && index + 1 < argc) {
            targets_path = argv[++index];
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[index]);
            return 2;
        }
    }
    if (token.empty()) {
        token = random_token();
    }
    if (token.size() < 32) {
        std::fprintf(stderr, "token is too short\n");
        return 2;
    }
    if (targets_path.empty()) {
        std::fprintf(stderr, "usage: %s --targets PATH [--port 0] [--token HEX]\n", argv[0]);
        return 2;
    }
    return run_server(port, token, targets_path);
}
