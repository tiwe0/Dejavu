#include <android/dlext.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <string>

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
    const timeval timeout = {2, 0};
    if (!set_close_on_exec(fd) ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        !peer_matches_process(fd, expected_pid) ||
        !dejavu_control_read_authentication(fd, endpoint) ||
        !dejavu_control_write_frame(fd, nullptr, 0)) {
        close(fd);
        return -1;
    }
    const timeval no_timeout = {};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &no_timeout, sizeof(no_timeout)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
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
    DejavuControlEndpoint endpoint = {};
    const int listener = dejavu_control_open_listener(&endpoint);
    if (listener < 0 || !dejavu_control_write_endpoint(client, endpoint)) {
        LOGE("companion failed to open control socket for %s", client_info.process_name.c_str());
        if (listener >= 0) {
            close(listener);
        }
        close(client);
        return;
    }
    close(client);
    LOGI(
        "companion waiting on @%s for %s pid %d",
        endpoint.socket_name,
        path.c_str(),
        static_cast<int>(client_info.pid));

    int control = -1;
    while (process_is_alive(client_info.pid)) {
        pollfd descriptor = {listener, POLLIN, 0};
        const int status = poll(&descriptor, 1, 250);
        if (status < 0 && errno == EINTR) {
            continue;
        }
        if (status < 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            break;
        }
        if (status == 0 || (descriptor.revents & POLLIN) == 0) {
            continue;
        }
        control = accept_control_client(listener, endpoint, client_info.pid);
        if (control >= 0) {
            break;
        }
        LOGE("companion rejected control client for pid %d", static_cast<int>(client_info.pid));
    }
    close(listener);
    if (control < 0) {
        LOGE("companion control connection unavailable for pid %d", static_cast<int>(client_info.pid));
        return;
    }
    LOGI("companion authenticated control client for pid %d", static_cast<int>(client_info.pid));

    FileIdentity observed = file_identity(path);
    while (process_is_alive(client_info.pid)) {
        poll(nullptr, 0, 250);
        const FileIdentity current = file_identity(path);
        if (!current.exists || current == observed) {
            continue;
        }
        std::string script;
        if (!read_control_file(path, &script) ||
            !dejavu_control_write_frame(control, script.data(), script.size())) {
            break;
        }
        observed = current;
    }
    close(control);
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
