#include "control_channel.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <memory>
#include <string_view>
#include <utility>

#include "cJSON.h"

namespace {

using namespace std::chrono_literals;

class FileDescriptor {
  public:
    explicit FileDescriptor(int fd) : fd_(fd) {
    }
    ~FileDescriptor() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }
    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;
    int get() const {
        return fd_;
    }

  private:
    int fd_;
};

int read_retry(int fd, char *buffer, size_t size) {
    ssize_t count;
    do {
        count = read(fd, buffer, size);
    } while (count < 0 && errno == EINTR);
    return static_cast<int>(count);
}

bool process_matches(int proc_fd, std::string_view process) {
    FileDescriptor cmdline(openat(proc_fd, "cmdline", O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    char name[129];
    const int count = read_retry(cmdline.get(), name, sizeof(name));
    return count > static_cast<int>(process.size()) && name[process.size()] == '\0' &&
           memcmp(process.data(), name, process.size()) == 0;
}

int hex_digit(char digit) {
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f') {
        return digit - 'a' + 10;
    }
    return -1;
}

bool read_endpoint(const std::string &path, pid_t pid, DejavuControlEndpoint *endpoint) {
    const size_t filename_start = path.rfind('/') + 1;
    FileDescriptor directory(open(path.substr(0, filename_start - 1).c_str(),
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    struct stat info {};
    if (fstat(directory.get(), &info) != 0 || info.st_uid != 0 || (info.st_mode & 022) != 0) {
        return false;
    }
    FileDescriptor metadata(openat(directory.get(), path.c_str() + filename_start,
                                   O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    char json[1025];
    if (fstat(metadata.get(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != 0 ||
        (info.st_mode & 077) != 0 || info.st_size <= 0 ||
        info.st_size >= static_cast<off_t>(sizeof(json))) {
        return false;
    }
    size_t size = 0;
    while (size < sizeof(json)) {
        const int count = read_retry(metadata.get(), json + size, sizeof(json) - size);
        if (count < 0) {
            return false;
        }
        if (count == 0) {
            break;
        }
        size += static_cast<size_t>(count);
    }
    if (size != static_cast<size_t>(info.st_size) || memchr(json, '\0', size) != nullptr) {
        return false;
    }
    json[size] = '\0';
    const std::unique_ptr<cJSON, decltype(&cJSON_Delete)> object(
        cJSON_ParseWithLengthOpts(json, size + 1, nullptr, 1), cJSON_Delete);
    if (!cJSON_IsObject(object.get()) || cJSON_GetArraySize(object.get()) != 4) {
        return false;
    }
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(object.get(), "version");
    const cJSON *stored_pid = cJSON_GetObjectItemCaseSensitive(object.get(), "pid");
    const char *socket =
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object.get(), "socket"));
    const char *token =
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object.get(), "token"));
    if (!cJSON_IsNumber(version) || version->valuedouble != 1 || !cJSON_IsNumber(stored_pid) ||
        stored_pid->valuedouble != pid || socket == nullptr || token == nullptr) {
        return false;
    }
    constexpr std::string_view prefix = "dejavu-";
    const std::string_view socket_name(socket);
    if (!socket_name.starts_with(prefix) || socket_name.size() != prefix.size() + 32 ||
        !std::ranges::all_of(socket_name.substr(prefix.size()),
                             [](char digit) { return hex_digit(digit) >= 0; }) ||
        strlen(token) != sizeof(endpoint->token) * 2) {
        return false;
    }
    memcpy(endpoint->socket_name, socket, socket_name.size() + 1);
    for (size_t index = 0; index < sizeof(endpoint->token); ++index) {
        const int high = hex_digit(token[index * 2]);
        const int low = hex_digit(token[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        endpoint->token[index] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

void exchange(std::string_view process, pid_t pid, const std::string &metadata,
              const DejavuControlRpcMessage &request, DejavuControlRpcMessage *response,
              DejavuControlDeadline deadline) {
    const std::string proc_path = "/proc/" + std::to_string(pid);
    const FileDescriptor proc(
        open(proc_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!process_matches(proc.get(), process)) {
        response->payload = "RPC target process is no longer running";
        return;
    }
    DejavuControlEndpoint endpoint;
    if (!read_endpoint(metadata, pid, &endpoint)) {
        response->payload = "RPC endpoint metadata is unavailable";
        return;
    }
    const auto connect_deadline = std::min(deadline, std::chrono::steady_clock::now() + 3s);
    const FileDescriptor connection(dejavu_control_connect(endpoint, connect_deadline));
    if (connection.get() < 0 || !dejavu_control_peer_is_root(connection.get())) {
        response->payload = "Unable to connect to Dejavu RPC companion";
        return;
    }
    std::string ack;
    if (!dejavu_control_write_authentication(connection.get(), endpoint, connect_deadline) ||
        !dejavu_control_read_frame(connection.get(), &ack, 0, connect_deadline)) {
        response->payload = "Dejavu RPC companion rejected authentication";
        return;
    }
    if (!process_matches(proc.get(), process)) {
        response->payload = "RPC target process is no longer running";
        return;
    }
    DejavuControlRpcMessage reply;
    if (!dejavu_control_write_rpc(connection.get(), request, deadline) ||
        !dejavu_control_read_rpc(connection.get(), &reply, deadline) ||
        reply.type != response->type || reply.request_id != request.request_id) {
        response->payload = "RPC response unavailable; request may have executed";
        return;
    }
    *response = std::move(reply);
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 3 || geteuid() != 0 || signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        return 1;
    }
    const std::string process = argv[1];
    const std::string_view pid_text(argv[2]);
    const char *pid_end = pid_text.data() + pid_text.size();
    pid_t pid = 0;
    const auto parsed = std::from_chars(pid_text.data(), pid_end, pid);
    std::string path;
    if (parsed.ec != std::errc{} || parsed.ptr != pid_end || pid <= 0 ||
        !dejavu_control_file_path(process, &path)) {
        return 1;
    }
    path.replace(path.size() - 4, 4, "." + std::to_string(pid) + ".rpc");

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + 38s;
    DejavuControlRpcMessage request;
    if (!dejavu_control_read_rpc_pipe(STDIN_FILENO, &request, start + 3s) ||
        request.type != DejavuControlRpcType::kExecuteRequest) {
        return 1;
    }
    DejavuControlRpcMessage response{
        DejavuControlRpcType::kExecuteResponse,
        request.request_id,
        DEJAVU_CONTROL_RPC_UNAVAILABLE,
        {},
    };
    exchange(process, pid, path, request, &response, deadline - 2s);
    return dejavu_control_write_rpc_pipe(STDOUT_FILENO, response,
                                         std::min(deadline, std::chrono::steady_clock::now() + 2s))
               ? 0
               : 1;
}
