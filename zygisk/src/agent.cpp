#include "dejavu_agent_api.h"

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <new>
#include <string>
#include <thread>

#include "control_channel.h"
#include "dejavu_runtime.h"
#include "hook_manager.h"
#include "lsplant_runtime.h"

namespace {

constexpr char kLogTag[] = "DejavuAgent";

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, kLogTag, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)

bool read_text_file_at(
    int directory_fd,
    const char *path,
    std::string *text,
    std::string *error) {
    struct stat status = {};
    const int fd = openat(directory_fd, path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error->assign(strerror(errno));
        return false;
    }
    if (fstat(fd, &status) != 0 || status.st_size < 0 ||
        status.st_size > static_cast<off_t>(kDejavuMaximumControlScriptSize)) {
        close(fd);
        error->assign("invalid init.lua size");
        return false;
    }
    text->resize(static_cast<size_t>(status.st_size));
    size_t offset = 0;
    while (offset < text->size()) {
        const ssize_t count = read(fd, text->data() + offset, text->size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            close(fd);
            error->assign(count == 0 ? "truncated init.lua" : strerror(errno));
            return false;
        }
        offset += static_cast<size_t>(count);
    }
    close(fd);
    return true;
}

class DejavuAgent final {
public:
    explicit DejavuAgent(const DejavuAgentCreateArgs &args)
        : env_(args.env), vm_(args.vm), control_endpoint_(args.control_endpoint),
          process_name_(args.process_name) {
        initialize(args.module_dir_fd);
    }

    void post_app_specialize() {
        if (!ready_) {
            return;
        }
        LOGD("agent active for %s; Dejavu %s", process_name_.c_str(), runtime_.version());
        std::string error;
        if (!hook_manager_.initialize(env_, vm_, lsplant_backend_, &runtime_, &error)) {
            LOGE("HookManager initialization failed: %s", error.c_str());
            return;
        }
        if (!hook_manager_.install_app_bootstrap(env_, application_attached, this, &error)) {
            LOGE("app bootstrap hook failed: %s", error.c_str());
            return;
        }
        LOGD("ActivityThread.handleBindApplication bootstrap hook installed");
#if DEJAVU_RUN_DEVICE_SMOKE
        const int status = runtime_.smoke();
        if (status == DEJAVU_OK) {
            LOGD("device smoke passed");
        } else {
            LOGE("device smoke failed with status %d", status);
        }
#endif
    }

private:
    JNIEnv *env_ = nullptr;
    JavaVM *vm_ = nullptr;
    DejavuControlEndpoint control_endpoint_ = {};
    std::string process_name_;
    DejavuRuntime runtime_ = {};
    const DejavuLsplantBackend *lsplant_backend_ = nullptr;
    HookManager hook_manager_;
    std::string init_script_;
    bool ready_ = false;

    void initialize(int module_dir_fd) {
        char error[256];
        if (!dejavu_runtime_load(module_dir_fd, &runtime_, error, sizeof(error))) {
            LOGE("failed to load Dejavu for %s: %s", process_name_.c_str(), error);
            return;
        }

        const LsplantRuntimeResult lsplant = lsplant_runtime_initialize(env_);
        if (lsplant.status != LsplantStatus::kReady || lsplant.backend == nullptr) {
            LOGE("LSPlant initialization failed for %s", process_name_.c_str());
            return;
        }
        lsplant_backend_ = lsplant.backend;

        std::string script_error;
        if (!read_text_file_at(
                module_dir_fd, "config/init.lua", &init_script_, &script_error)) {
            LOGE(
                "failed to read config/init.lua for %s: %s",
                process_name_.c_str(),
                script_error.c_str());
            return;
        }
        ready_ = true;
    }

    static void application_attached(JNIEnv *env, jobject class_loader, void *context) {
        auto *agent = static_cast<DejavuAgent *>(context);
        std::string error;
        if (!agent->hook_manager_.start_control(
                env, class_loader, agent->init_script_, &error)) {
            LOGE("Lua control initialization failed: %s", error.c_str());
            return;
        }
        LOGD("persistent Lua control initialized");
        agent->start_live_control();
    }

    void start_live_control() {
        if (control_endpoint_.socket_name[0] == '\0') {
            LOGD("live control unavailable: socket endpoint not established");
            return;
        }
        std::thread([this]() {
            const int control = dejavu_control_connect(control_endpoint_);
            if (control < 0) {
                LOGE("live control connect failed: %s", strerror(errno));
                return;
            }
            std::string acknowledgement;
            if (!dejavu_control_write_authentication(control, control_endpoint_) ||
                !dejavu_control_read_frame(control, &acknowledgement, 0)) {
                LOGE("live control authentication failed");
                close(control);
                return;
            }
            LOGD("abstract-socket live control connected");
            std::string script;
            while (dejavu_control_read_frame(
                control, &script, kDejavuMaximumControlScriptSize)) {
                std::string error;
                if (hook_manager_.eval_control(script, &error)) {
                    LOGD("live control applied: %zu bytes", script.size());
                } else {
                    LOGE("live control failed: %s", error.c_str());
                }
            }
            close(control);
            LOGD("abstract-socket live control stopped");
        }).detach();
        LOGD("abstract-socket live control starting");
    }
};

void *create_agent(const DejavuAgentCreateArgs *args) {
    if (args == nullptr || args->struct_size < sizeof(DejavuAgentCreateArgs) ||
        args->abi_version != DEJAVU_AGENT_ABI_VERSION || args->env == nullptr ||
        args->vm == nullptr || args->module_dir_fd < 0 || args->process_name == nullptr) {
        return nullptr;
    }
    return new (std::nothrow) DejavuAgent(*args);
}

void post_app_specialize(void *instance) {
    if (instance != nullptr) {
        static_cast<DejavuAgent *>(instance)->post_app_specialize();
    }
}

const DejavuAgentApi kAgentApi = {
    sizeof(DejavuAgentApi),
    DEJAVU_AGENT_ABI_VERSION,
    create_agent,
    post_app_specialize,
};

}  // namespace

extern "C" __attribute__((visibility("default")))
const DejavuAgentApi *dejavu_agent_get_api(uint32_t abi_version) {
    return abi_version == DEJAVU_AGENT_ABI_VERSION ? &kAgentApi : nullptr;
}
