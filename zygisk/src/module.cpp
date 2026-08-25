#include <android/dlext.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <string>
#include <utility>

#include "control_channel.h"
#include "dejavu_agent_api.h"
#include "target_config.h"
#include "zygisk.hpp"

namespace {

constexpr char kLogTag[] = "DejavuZygisk";
constexpr char kAgentPath[] = "lib/arm64-v8a/libdejavu_agent.so";

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, kLogTag, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, kLogTag, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)

template <typename Function>
bool resolve(void *handle, const char *name, Function *function) {
    dlerror();
    void *symbol = dlsym(handle, name);
    if (symbol == nullptr || dlerror() != nullptr) {
        return false;
    }
    static_assert(sizeof(*function) == sizeof(symbol));
    memcpy(function, &symbol, sizeof(symbol));
    return true;
}

struct FileIdentity {
    ino_t inode = 0;
    off_t size = 0;
    time_t seconds = 0;
    long nanoseconds = 0;
    bool exists = false;
};

FileIdentity file_identity(const std::string &path) {
    struct stat status = {};
    if (stat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0 ||
        status.st_size > static_cast<off_t>(kDejavuMaximumControlScriptSize)) {
        return {};
    }
    return {
        status.st_ino,
        status.st_size,
        status.st_mtim.tv_sec,
        status.st_mtim.tv_nsec,
        true,
    };
}

bool operator==(const FileIdentity &left, const FileIdentity &right) {
    return left.inode == right.inode && left.size == right.size &&
        left.seconds == right.seconds && left.nanoseconds == right.nanoseconds &&
        left.exists == right.exists;
}

bool read_control_file(const std::string &path, std::string *script) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    struct stat status = {};
    if (fstat(fd, &status) != 0 || status.st_size < 0 ||
        status.st_size > static_cast<off_t>(kDejavuMaximumControlScriptSize)) {
        close(fd);
        return false;
    }
    script->resize(static_cast<size_t>(status.st_size));
    size_t offset = 0;
    while (offset < script->size()) {
        const ssize_t count = read(fd, script->data() + offset, script->size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            close(fd);
            return false;
        }
        offset += static_cast<size_t>(count);
    }
    close(fd);
    return true;
}

bool process_is_alive(pid_t pid) {
    return kill(pid, 0) == 0 || errno == EPERM;
}

bool set_close_on_exec(int fd) {
    const int flags = fcntl(fd, F_GETFD);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

bool set_receive_timeout(int fd, int seconds) {
    const timeval timeout = {seconds, 0};
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0;
}

bool peer_matches_process(int fd, pid_t expected_pid) {
    struct ucred credentials = {};
    socklen_t size = sizeof(credentials);
    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0 &&
        size == sizeof(credentials) && credentials.pid == expected_pid;
}

int accept_control_client(
    int listener,
    const DejavuControlEndpoint &endpoint,
    pid_t expected_pid) {
    const int fd = accept(listener, nullptr, nullptr);
    if (fd < 0) {
        return -1;
    }
    if (!set_close_on_exec(fd) ||
        !set_receive_timeout(fd, 2) ||
        (expected_pid > 0 && !peer_matches_process(fd, expected_pid)) ||
        !dejavu_control_read_authentication(fd, endpoint) ||
        !dejavu_control_write_frame(fd, nullptr, 0)) {
        close(fd);
        return -1;
    }
    if (!set_receive_timeout(fd, 0)) {
        close(fd);
        return -1;
    }
    return fd;
}

bool endpoint_metadata_path(
    const DejavuControlClientInfo &client_info,
    std::string *path) {
    std::string script_path;
    if (path == nullptr ||
        !dejavu_control_file_path(client_info.process_name, &script_path) ||
        script_path.size() < 4) {
        return false;
    }
    script_path.resize(script_path.size() - 4);
    *path = script_path + "." + std::to_string(client_info.pid) + ".rpc";
    return true;
}

bool write_endpoint_metadata(
    const std::string &path,
    pid_t pid,
    const DejavuControlEndpoint &endpoint) {
    constexpr char hex[] = "0123456789abcdef";
    char token[kDejavuControlTokenSize * 2 + 1] = {};
    for (size_t index = 0; index < sizeof(endpoint.token); ++index) {
        token[index * 2] = hex[endpoint.token[index] >> 4];
        token[index * 2 + 1] = hex[endpoint.token[index] & 0x0f];
    }
    const std::string metadata =
        "{\"version\":1,\"pid\":" + std::to_string(pid) +
        ",\"socket\":\"" + endpoint.socket_name +
        "\",\"token\":\"" + token + "\"}\n";
    const std::string temporary = path + ".new";
    const int fd = open(
        temporary.c_str(),
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
        0600);
    if (fd < 0) {
        return false;
    }
    size_t offset = 0;
    while (offset < metadata.size()) {
        const ssize_t count = write(fd, metadata.data() + offset, metadata.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            close(fd);
            unlink(temporary.c_str());
            return false;
        }
        offset += static_cast<size_t>(count);
    }
    const bool permissions_set = fchmod(fd, 0600) == 0;
    const bool closed = close(fd) == 0;
    const bool written = permissions_set && closed &&
        rename(temporary.c_str(), path.c_str()) == 0;
    if (!written) {
        unlink(temporary.c_str());
    }
    return written;
}

DejavuControlRpcMessage rpc_error(
    uint64_t request_id,
    int32_t status,
    const char *message) {
    return {
        DejavuControlRpcType::kExecuteResponse,
        request_id,
        status,
        message,
    };
}

bool relay_rpc(
    int *agent_control,
    const DejavuControlRpcMessage &request,
    DejavuControlRpcMessage *response) {
    if (agent_control == nullptr || response == nullptr) {
        return false;
    }
    if (request.type == DejavuControlRpcType::kReconnectRequest) {
        if (!request.payload.empty()) {
            *response = {
                DejavuControlRpcType::kReconnectResponse,
                request.request_id,
                DEJAVU_CONTROL_RPC_PROTOCOL_ERROR,
                "reconnect request payload must be empty",
            };
            return true;
        }
        if (*agent_control >= 0) {
            close(*agent_control);
            *agent_control = -1;
        }
        *response = {
            DejavuControlRpcType::kReconnectResponse,
            request.request_id,
            DEJAVU_CONTROL_RPC_OK,
            "Agent reconnect requested",
        };
        return true;
    }
    if (request.type != DejavuControlRpcType::kExecuteRequest) {
        *response = rpc_error(
            request.request_id,
            DEJAVU_CONTROL_RPC_PROTOCOL_ERROR,
            "execute request required");
        return true;
    }
    if (*agent_control < 0) {
        *response = rpc_error(
            request.request_id,
            DEJAVU_CONTROL_RPC_UNAVAILABLE,
            "target Agent is not connected");
        return true;
    }
    DejavuControlRpcMessage agent_response;
    if (!dejavu_control_write_rpc(*agent_control, request) ||
        !dejavu_control_read_rpc(*agent_control, &agent_response) ||
        agent_response.type != DejavuControlRpcType::kExecuteResponse ||
        agent_response.request_id != request.request_id) {
        close(*agent_control);
        *agent_control = -1;
        *response = rpc_error(
            request.request_id,
            DEJAVU_CONTROL_RPC_UNAVAILABLE,
            "target Agent disconnected; retry the command");
        return true;
    }
    *response = std::move(agent_response);
    return true;
}

void handle_cli_client(int client, int *agent_control) {
    if (!set_receive_timeout(client, 30)) {
        close(client);
        return;
    }
    DejavuControlRpcMessage request;
    DejavuControlRpcMessage response;
    if (dejavu_control_read_rpc(client, &request) &&
        relay_rpc(agent_control, request, &response)) {
        dejavu_control_write_rpc(client, response);
    }
    close(client);
}

void companion_handler(int client) {
    LOGI("companion connected: fd=%d", client);
    DejavuControlClientInfo client_info;
    if (!dejavu_control_read_client_info(client, &client_info)) {
        LOGE("companion failed to read client info: fd=%d", client);
        close(client);
        return;
    }
    std::string path;
    if (!dejavu_control_file_path(client_info.process_name, &path)) {
        LOGE("companion rejected process name: %s", client_info.process_name.c_str());
        close(client);
        return;
    }
    std::string metadata_path;
    DejavuControlEndpoint agent_endpoint = {};
    DejavuControlEndpoint cli_endpoint = {};
    const int agent_listener = dejavu_control_open_listener(&agent_endpoint);
    const int cli_listener = dejavu_control_open_listener(&cli_endpoint);
    if (agent_listener < 0 || cli_listener < 0 ||
        !endpoint_metadata_path(client_info, &metadata_path) ||
        !write_endpoint_metadata(metadata_path, client_info.pid, cli_endpoint) ||
        !dejavu_control_write_endpoint(client, agent_endpoint)) {
        LOGE("companion failed to open RPC sockets for %s", client_info.process_name.c_str());
        if (agent_listener >= 0) {
            close(agent_listener);
        }
        if (cli_listener >= 0) {
            close(cli_listener);
        }
        if (!metadata_path.empty()) {
            unlink(metadata_path.c_str());
        }
        close(client);
        return;
    }
    close(client);
    LOGI(
        "companion RPC ready: agent=@%s cli=@%s pid=%d",
        agent_endpoint.socket_name,
        cli_endpoint.socket_name,
        static_cast<int>(client_info.pid));
    LOGI(
        "companion monitoring %s",
        path.c_str());

    int agent_control = -1;
    uint64_t next_file_request_id = 1;
    FileIdentity observed = file_identity(path);
    while (process_is_alive(client_info.pid)) {
        pollfd descriptors[] = {
            {agent_listener, POLLIN, 0},
            {cli_listener, POLLIN, 0},
            {agent_control, POLLIN, 0},
        };
        const int descriptor_count = agent_control >= 0 ? 3 : 2;
        const int status = poll(descriptors, descriptor_count, 250);
        if (status < 0 && errno == EINTR) {
            continue;
        }
        if (status < 0 ||
            (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 ||
            (descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            break;
        }
        if (agent_control >= 0 &&
            (descriptors[2].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0) {
            close(agent_control);
            agent_control = -1;
            LOGI(
                "companion detected Agent disconnect for pid %d",
                static_cast<int>(client_info.pid));
        }
        if ((descriptors[0].revents & POLLIN) != 0) {
            const int candidate = accept_control_client(
                agent_listener, agent_endpoint, client_info.pid);
            if (candidate >= 0) {
                if (agent_control >= 0) {
                    close(agent_control);
                }
                agent_control = candidate;
                LOGI(
                    "companion authenticated Agent for pid %d",
                    static_cast<int>(client_info.pid));
            } else {
                LOGE(
                    "companion rejected Agent for pid %d",
                    static_cast<int>(client_info.pid));
            }
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            const int cli = accept_control_client(cli_listener, cli_endpoint, -1);
            if (cli >= 0) {
                handle_cli_client(cli, &agent_control);
            } else {
                LOGE("companion rejected CLI authentication");
            }
        }
        const FileIdentity current = file_identity(path);
        if (!current.exists || current == observed) {
            continue;
        }
        std::string script;
        if (!read_control_file(path, &script)) {
            break;
        }
        const DejavuControlRpcMessage request = {
            DejavuControlRpcType::kExecuteRequest,
            next_file_request_id++,
            DEJAVU_CONTROL_RPC_OK,
            std::move(script),
        };
        DejavuControlRpcMessage response;
        if (!relay_rpc(&agent_control, request, &response)) {
            break;
        }
        if (response.status == DEJAVU_CONTROL_RPC_UNAVAILABLE) {
            continue;
        }
        observed = current;
        if (response.status == DEJAVU_CONTROL_RPC_OK) {
            LOGI(
                "file control applied: request=%llu",
                static_cast<unsigned long long>(request.request_id));
        } else {
            LOGE("file control failed: %s", response.payload.c_str());
        }
    }
    if (agent_control >= 0) {
        close(agent_control);
    }
    close(agent_listener);
    close(cli_listener);
    unlink(metadata_path.c_str());
    LOGI("companion stopped monitoring pid %d", static_cast<int>(client_info.pid));
}

class DejavuZygiskModule final : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        api_ = api;
        env_ = env;
        if (env_ != nullptr) {
            const jint status = env_->GetJavaVM(&vm_);
            if (status != JNI_OK) {
                LOGE("onLoad GetJavaVM failed: status=%d", status);
                vm_ = nullptr;
            }
        }
        LOGD("onLoad: api=%p env=%p vm=%p", api_, env_, vm_);
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        if (api_ == nullptr || env_ == nullptr || vm_ == nullptr || args == nullptr ||
            args->nice_name == nullptr) {
            LOGE(
                "preAppSpecialize rejected invalid state: api=%p env=%p vm=%p args=%p",
                api_,
                env_,
                vm_,
                args);
            unload_self();
            return;
        }

        const char *process_name = env_->GetStringUTFChars(args->nice_name, nullptr);
        if (process_name == nullptr) {
            LOGE("preAppSpecialize failed to read process name");
            unload_self();
            return;
        }
        const int module_dir_fd = api_->getModuleDir();
        selected_ = target_config_matches(module_dir_fd, process_name);
        if (selected_) {
            LOGI("target selected: process=%s module_dir_fd=%d", process_name, module_dir_fd);
        } else {
            LOGD("target skipped: process=%s module_dir_fd=%d", process_name, module_dir_fd);
        }
        if (selected_) {
            load_agent(module_dir_fd, process_name);
        }
        if (module_dir_fd >= 0) {
            close(module_dir_fd);
        }
        env_->ReleaseStringUTFChars(args->nice_name, process_name);
        if (!selected_) {
            unload_self();
        }
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (agent_api_ != nullptr && agent_instance_ != nullptr) {
            LOGI("calling Agent postAppSpecialize");
            agent_api_->post_app_specialize(agent_instance_);
        } else if (selected_) {
            LOGE("selected target has no Agent instance in postAppSpecialize");
        }
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs *) override {
        unload_self();
    }

private:
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    JavaVM *vm_ = nullptr;
    void *agent_library_ = nullptr;
    const DejavuAgentApi *agent_api_ = nullptr;
    void *agent_instance_ = nullptr;
    bool selected_ = false;

    bool connect_control(
        const char *process_name,
        DejavuControlEndpoint *endpoint) {
        if (endpoint == nullptr) {
            return false;
        }
        *endpoint = {};
        int fd = api_->connectCompanion();
        if (fd < 0) {
            LOGE("connectCompanion failed for %s: fd=%d", process_name, fd);
            return false;
        }
        const bool connected =
            dejavu_control_write_client_info(fd, getpid(), process_name) &&
            dejavu_control_read_endpoint(fd, endpoint);
        close(fd);
        if (!connected) {
            LOGE("companion did not provide a control endpoint for %s", process_name);
            *endpoint = {};
            return false;
        }
        LOGI("control endpoint received for %s: @%s", process_name, endpoint->socket_name);
        return true;
    }

    void load_agent(int module_dir_fd, const char *process_name) {
        LOGI("opening Agent for %s at %s", process_name, kAgentPath);
        const int library_fd = openat(module_dir_fd, kAgentPath, O_RDONLY | O_CLOEXEC);
        if (library_fd < 0) {
            LOGE("unable to open Agent for %s: %s", process_name, strerror(errno));
            return;
        }
        android_dlextinfo info = {};
        info.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
        info.library_fd = library_fd;
        agent_library_ = android_dlopen_ext(
            "libdejavu_agent.so", RTLD_NOW | RTLD_LOCAL, &info);
        close(library_fd);
        if (agent_library_ == nullptr) {
            LOGE("unable to load Agent for %s: %s", process_name, dlerror());
            return;
        }

        DejavuAgentGetApi get_api = nullptr;
        if (!resolve(agent_library_, "dejavu_agent_get_api", &get_api) || get_api == nullptr) {
            LOGE("Agent ABI entry is unavailable for %s", process_name);
            return;
        }
        agent_api_ = get_api(DEJAVU_AGENT_ABI_VERSION);
        if (agent_api_ == nullptr || agent_api_->struct_size < sizeof(DejavuAgentApi) ||
            agent_api_->abi_version != DEJAVU_AGENT_ABI_VERSION || agent_api_->create == nullptr ||
            agent_api_->post_app_specialize == nullptr) {
            LOGE("Agent ABI mismatch for %s", process_name);
            agent_api_ = nullptr;
            return;
        }

        DejavuControlEndpoint control_endpoint = {};
        connect_control(process_name, &control_endpoint);
        const DejavuAgentCreateArgs create_args = {
            sizeof(DejavuAgentCreateArgs),
            DEJAVU_AGENT_ABI_VERSION,
            env_,
            vm_,
            module_dir_fd,
            control_endpoint,
            process_name,
        };
        agent_instance_ = agent_api_->create(&create_args);
        if (agent_instance_ == nullptr) {
            LOGE("Agent creation failed for %s", process_name);
            agent_api_ = nullptr;
            return;
        }
        LOGD("per-process Agent loaded for %s", process_name);
    }

    void unload_self() {
        if (api_ != nullptr) {
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
        }
    }
};

}  // namespace

REGISTER_ZYGISK_MODULE(DejavuZygiskModule)
REGISTER_ZYGISK_COMPANION(companion_handler)
