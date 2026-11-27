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
#include <system_error>
#include <thread>
#include <utility>

#include "control_channel.h"
#include "dejavu_agent_api.h"
#include "target_config.h"
#include "zygisk.hpp"

namespace {

constexpr char kLogTag[] = "DejavuZygisk";
constexpr char kAgentPath[] = "lib/arm64-v8a/libdejavu_agent.so";
constexpr int kAgentRelayTimeoutSeconds = 30;
constexpr int kClientIoTimeoutSeconds = 2;
constexpr int kCompanionHandshakeTimeoutSeconds = 5;
constexpr int kMaxAgentDeniedRetries = 20;

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, kLogTag, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, kLogTag, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)

template <typename Function> bool resolve(void *handle, const char *name, Function *function) {
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

FileIdentity file_identity(const struct stat &status) {
    if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
        status.st_size > static_cast<off_t>(kDejavuMaximumControlScriptSize)) {
        return {};
    }
    return {
        status.st_ino, status.st_size, status.st_mtim.tv_sec, status.st_mtim.tv_nsec, true,
    };
}

FileIdentity file_identity(const std::string &path) {
    struct stat status = {};
    return stat(path.c_str(), &status) == 0 ? file_identity(status) : FileIdentity{};
}

bool operator==(const FileIdentity &left, const FileIdentity &right) {
    return left.inode == right.inode && left.size == right.size && left.seconds == right.seconds &&
           left.nanoseconds == right.nanoseconds && left.exists == right.exists;
}

bool read_control_file(const std::string &path, std::string *script, FileIdentity *identity) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        return false;
    }
    struct stat status = {};
    if (fstat(fd, &status) != 0 || !file_identity(status).exists) {
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
    struct stat after = {};
    const bool unchanged = fstat(fd, &after) == 0 && file_identity(status) == file_identity(after);
    close(fd);
    if (unchanged) {
        *identity = file_identity(status);
    }
    return unchanged;
}

bool process_is_alive(pid_t pid) {
    if (kill(pid, 0) != 0 && errno != EPERM) {
        return false;
    }
    char stat_path[64];
    snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", static_cast<int>(pid));
    const int fd = open(stat_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return errno != ENOENT && errno != ESRCH;
    }
    char buffer[256];
    ssize_t count = -1;
    do {
        count = read(fd, buffer, sizeof(buffer) - 1);
    } while (count < 0 && errno == EINTR);
    close(fd);
    if (count <= 0) {
        return false;
    }
    buffer[count] = '\0';
    const char *rparen = strrchr(buffer, ')');
    if (rparen != nullptr && rparen[1] == ' ' && rparen[2] != '\0') {
        const char state = rparen[2];
        if (state == 'Z' || state == 'X' || state == 'x') {
            return false;
        }
    }
    return true;
}

bool set_close_on_exec(int fd) {
    const int flags = fcntl(fd, F_GETFD);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

int accept_cli_client(int listener, const DejavuControlEndpoint &endpoint) {
    const int fd = accept(listener, nullptr, nullptr);
    if (fd < 0) {
        return -1;
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(kClientIoTimeoutSeconds);
    if (!set_close_on_exec(fd) || !dejavu_control_read_authentication(fd, endpoint, deadline) ||
        !dejavu_control_write_frame(fd, nullptr, 0, deadline)) {
        close(fd);
        return -1;
    }
    return fd;
}

bool endpoint_metadata_path(const DejavuControlClientInfo &client_info, std::string *path) {
    std::string script_path;
    if (path == nullptr || !dejavu_control_file_path(client_info.process_name, &script_path) ||
        script_path.size() < 4) {
        return false;
    }
    script_path.resize(script_path.size() - 4);
    *path = script_path + "." + std::to_string(client_info.pid) + ".rpc";
    return true;
}

bool write_endpoint_metadata(const std::string &path, pid_t pid,
                             const DejavuControlEndpoint &endpoint) {
    constexpr char hex[] = "0123456789abcdef";
    char token[kDejavuControlTokenSize * 2 + 1] = {};
    for (size_t index = 0; index < sizeof(endpoint.token); ++index) {
        token[index * 2] = hex[endpoint.token[index] >> 4];
        token[index * 2 + 1] = hex[endpoint.token[index] & 0x0f];
    }
    const std::string metadata = "{\"version\":1,\"pid\":" + std::to_string(pid) +
                                 ",\"socket\":\"" + endpoint.socket_name + "\",\"token\":\"" +
                                 token + "\"}\n";
    const std::string temporary = path + ".new";
    const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
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
    const bool written = permissions_set && closed && rename(temporary.c_str(), path.c_str()) == 0;
    if (!written) {
        unlink(temporary.c_str());
    }
    return written;
}

void handle_cli_client(int client, int *agent_control) {
    DejavuControlRpcMessage request;
    DejavuControlRpcMessage response;
    const auto read_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(kClientIoTimeoutSeconds);
    if (dejavu_control_read_rpc(client, &request, read_deadline) &&
        dejavu_control_relay_rpc(agent_control, request, &response,
                                 std::chrono::steady_clock::now() +
                                     std::chrono::seconds(kAgentRelayTimeoutSeconds))) {
        dejavu_control_write_rpc(client, response,
                                 std::chrono::steady_clock::now() +
                                     std::chrono::seconds(kClientIoTimeoutSeconds));
    }
    close(client);
}

int connect_agent(const DejavuControlEndpoint &endpoint, pid_t pid, uid_t uid) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(kCompanionHandshakeTimeoutSeconds);
    const int fd = dejavu_control_connect(endpoint, deadline);
    if (fd < 0) {
        return -1;
    }
    if (!dejavu_control_peer_matches(fd, pid, uid)) {
        close(fd);
        errno = EACCES;
        return -1;
    }
    std::string acknowledgement;
    errno = 0;
    if (!dejavu_control_write_authentication(fd, endpoint, deadline) ||
        !dejavu_control_read_frame(fd, &acknowledgement, 0, deadline)) {
        const int error = errno;
        close(fd);
        errno = error == 0 ? ECONNRESET : error;
        return -1;
    }
    return fd;
}

void monitor_target_companion(int cli_listener, DejavuControlEndpoint cli_endpoint,
                              DejavuControlEndpoint agent_endpoint,
                              DejavuControlClientInfo client_info, uid_t app_uid, std::string path,
                              std::string metadata_path) {
    LOGI("companion RPC ready: cli=@%s pid=%d", cli_endpoint.socket_name,
         static_cast<int>(client_info.pid));
    LOGI("companion monitoring %s", path.c_str());

    int agent_control = -1;
    bool agent_denied = false;
    int agent_denied_count = 0;
    uint64_t next_file_request_id = 1;
    FileIdentity observed = file_identity(path);
    while (process_is_alive(client_info.pid)) {
        pollfd descriptors[] = {
            {cli_listener, POLLIN, 0},
            {agent_control, POLLIN, 0},
        };
        const int status = poll(descriptors, 2, 250);
        if (status < 0 && errno == EINTR) {
            continue;
        }
        if (status < 0 || (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            break;
        }
        if (agent_control >= 0 && descriptors[1].revents != 0) {
            close(agent_control);
            agent_control = -1;
            LOGI("companion detected Agent disconnect for pid %d", client_info.pid);
        }
        if (agent_control < 0 && !agent_denied) {
            agent_control = connect_agent(agent_endpoint, client_info.pid, app_uid);
            if (agent_control >= 0) {
                agent_denied_count = 0;
                LOGI("companion authenticated Agent listener for pid %d", client_info.pid);
            } else if (errno == EACCES || errno == EPERM) {
                ++agent_denied_count;
                if (agent_denied_count == 1 || agent_denied_count >= kMaxAgentDeniedRetries) {
                    LOGE("companion Agent connection denied for pid %d (attempt %d/%d): %s",
                         client_info.pid, agent_denied_count, kMaxAgentDeniedRetries,
                         strerror(errno));
                }
                if (agent_denied_count >= kMaxAgentDeniedRetries) {
                    agent_denied = true;
                }
            }
        }
        if ((descriptors[0].revents & POLLIN) != 0) {
            const int cli = accept_cli_client(cli_listener, cli_endpoint);
            if (cli >= 0) {
                handle_cli_client(cli, &agent_control);
            } else {
                LOGE("companion rejected CLI authentication");
            }
        }
        const FileIdentity current = file_identity(path);
        if (agent_control < 0 || !current.exists || current == observed) {
            continue;
        }
        std::string script;
        FileIdentity read_identity;
        if (!read_control_file(path, &script, &read_identity) || read_identity == observed) {
            continue;
        }
        const DejavuControlRpcMessage request = {
            DejavuControlRpcType::kExecuteRequest,
            next_file_request_id++,
            DEJAVU_CONTROL_RPC_OK,
            std::move(script),
        };
        DejavuControlRpcMessage response;
        if (!dejavu_control_relay_rpc(&agent_control, request, &response,
                                      std::chrono::steady_clock::now() +
                                          std::chrono::seconds(kAgentRelayTimeoutSeconds))) {
            break;
        }
        // Retry a file only when it is certain that the Agent never received it.
        // A lost response must consume this version to avoid replaying mutations.
        if (response.status == DEJAVU_CONTROL_RPC_UNAVAILABLE &&
            response.payload == "target Agent is not connected") {
            continue;
        }
        observed = read_identity;
        if (response.status == DEJAVU_CONTROL_RPC_OK) {
            LOGI("file control applied: request=%llu",
                 static_cast<unsigned long long>(request.request_id));
        } else {
            LOGE("file control failed: %s", response.payload.c_str());
        }
    }
    if (agent_control >= 0) {
        close(agent_control);
    }
    close(cli_listener);
    unlink(metadata_path.c_str());
    LOGI("companion stopped monitoring pid %d", static_cast<int>(client_info.pid));
}

void companion_handler(int client) {
    LOGI("companion connected: fd=%d", client);
    const auto handshake_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(kCompanionHandshakeTimeoutSeconds);
    DejavuControlClientInfo client_info;
    const pid_t caller_pid = set_close_on_exec(client)
                                 ? dejavu_control_receive_caller_pid(client, handshake_deadline)
                                 : -1;
    std::string path;
    std::string metadata_path;
    std::string uid_frame;
    uid_t app_uid;
    if (caller_pid <= 0 ||
        !dejavu_control_read_client_info(client, &client_info, handshake_deadline) ||
        caller_pid != client_info.pid ||
        !dejavu_control_read_frame(client, &uid_frame, sizeof(app_uid), handshake_deadline) ||
        uid_frame.size() != sizeof(app_uid) ||
        !dejavu_control_file_path(client_info.process_name, &path) ||
        !endpoint_metadata_path(client_info, &metadata_path)) {
        LOGE("companion rejected client identity: fd=%d", client);
        close(client);
        return;
    }

    memcpy(&app_uid, uid_frame.data(), sizeof(app_uid));
    DejavuControlEndpoint agent_endpoint = {};
    if (!dejavu_control_generate_endpoint(&agent_endpoint)) {
        LOGE("companion failed to generate a private Agent endpoint");
        close(client);
        return;
    }
    DejavuControlEndpoint cli_endpoint = {};
    const int cli_listener = dejavu_control_open_listener(&cli_endpoint);
    if (cli_listener < 0 ||
        !write_endpoint_metadata(metadata_path, client_info.pid, cli_endpoint) ||
        !dejavu_control_write_endpoint(client, agent_endpoint, handshake_deadline)) {
        LOGE("companion failed to publish RPC endpoint for %s", client_info.process_name.c_str());
        if (cli_listener >= 0) {
            close(cli_listener);
        }
        unlink(metadata_path.c_str());
        close(client);
        return;
    }
    close(client);

    // Hand off long-lived monitoring to a detached thread so the Zygisk
    // companion worker returns immediately for concurrent app launches.
    try {
        std::thread(monitor_target_companion, cli_listener, cli_endpoint, agent_endpoint,
                    std::move(client_info), app_uid, std::move(path), std::move(metadata_path))
            .detach();
    } catch (const std::system_error &error) {
        LOGE("companion monitor thread creation failed (%s); running inline", error.what());
        monitor_target_companion(cli_listener, cli_endpoint, agent_endpoint, std::move(client_info),
                                 app_uid, std::move(path), std::move(metadata_path));
    }
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
            LOGE("preAppSpecialize rejected invalid state: api=%p env=%p vm=%p args=%p", api_, env_,
                 vm_, args);
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
            load_agent(module_dir_fd, process_name, args->uid);
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

    DejavuControlEndpoint request_control_endpoint(const char *process_name, uid_t app_uid) {
        const int fd = api_->connectCompanion();
        if (fd < 0) {
            LOGE("connectCompanion failed for %s: fd=%d", process_name, fd);
            return {};
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(kCompanionHandshakeTimeoutSeconds);
        DejavuControlEndpoint endpoint = {};
        const bool established =
            set_close_on_exec(fd) && dejavu_control_peer_is_root(fd) &&
            dejavu_control_prove_caller_pid(fd, deadline) &&
            dejavu_control_write_client_info(fd, getpid(), process_name, deadline) &&
            dejavu_control_write_frame(fd, &app_uid, sizeof(app_uid), deadline) &&
            dejavu_control_read_endpoint(fd, &endpoint, deadline);
        close(fd);
        if (!established) {
            LOGE("failed to negotiate Agent listener for %s", process_name);
            return {};
        }
        return endpoint;
    }

    void load_agent(int module_dir_fd, const char *process_name, uid_t app_uid) {
        LOGI("opening Agent for %s at %s", process_name, kAgentPath);
        const int library_fd = openat(module_dir_fd, kAgentPath, O_RDONLY | O_CLOEXEC);
        if (library_fd < 0) {
            LOGE("unable to open Agent for %s: %s", process_name, strerror(errno));
            return;
        }
        android_dlextinfo info = {};
        info.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
        info.library_fd = library_fd;
        agent_library_ = android_dlopen_ext("libdejavu_agent.so", RTLD_NOW | RTLD_LOCAL, &info);
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

        const auto control_endpoint = request_control_endpoint(process_name, app_uid);
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

} // namespace

REGISTER_ZYGISK_MODULE(DejavuZygiskModule)
REGISTER_ZYGISK_COMPANION(companion_handler)
