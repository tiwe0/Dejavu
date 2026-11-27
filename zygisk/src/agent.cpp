#include "dejavu_agent_api.h"

#include <android/log.h>
#include <errno.h>
#include <string.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "agent_startup_scripts.h"
#include "control_channel.h"
#include "dejavu_runtime.h"
#include "hook_manager.h"
#include "lsplant_runtime.h"

namespace {

constexpr char kLogTag[] = "DejavuAgent";

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, kLogTag, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)

class DejavuAgent final {
  public:
    explicit DejavuAgent(const DejavuAgentCreateArgs &args)
        : env_(args.env), vm_(args.vm), control_endpoint_(args.control_endpoint),
          process_name_(args.process_name) {
        initialize(args.module_dir_fd);
    }

    ~DejavuAgent() {
        if (!ready_) {
            dejavu_runtime_unload(&runtime_);
        }
    }

    bool is_ready() const {
        return ready_;
    }

    void post_app_specialize() {
        if (!ready_) {
            LOGE("post_app_specialize skipped: Agent is not ready for %s", process_name_.c_str());
            return;
        }
        JNIEnv *env = env_;
        if (vm_ != nullptr) {
            JNIEnv *current = nullptr;
            if (vm_->GetEnv(reinterpret_cast<void **>(&current), JNI_VERSION_1_6) == JNI_OK &&
                current != nullptr) {
                env = current;
            }
        }
        if (env == nullptr) {
            LOGE("post_app_specialize has no JNIEnv for %s", process_name_.c_str());
            return;
        }
        LOGD("agent active for %s; Dejavu %s", process_name_.c_str(), runtime_.version());
        std::string error;
        if (!hook_manager_.initialize(env, vm_, lsplant_backend_, &runtime_, &error)) {
            LOGE("HookManager initialization failed: %s", error.c_str());
            return;
        }
        if (!hook_manager_.install_app_bootstrap(env, application_attached, this, &error)) {
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
    std::string early_hook_script_;
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
        if (agent_read_script_at(module_dir_fd, "config/init.lua", &init_script_, &script_error) !=
            AgentScriptReadStatus::kLoaded) {
            LOGE("failed to read config/init.lua for %s: %s", process_name_.c_str(),
                 script_error.c_str());
            return;
        }
        const AgentScriptReadStatus early_status = agent_read_early_hook_at(
            module_dir_fd, process_name_, &early_hook_script_, &script_error);
        if (early_status == AgentScriptReadStatus::kError) {
            LOGE("failed to read early hook for %s: %s; continuing without it",
                 process_name_.c_str(), script_error.c_str());
        } else if (early_status == AgentScriptReadStatus::kLoaded) {
            LOGD("early hook loaded for %s: %zu bytes", process_name_.c_str(),
                 early_hook_script_.size());
        }
        ready_ = true;
    }

    static void application_attached(JNIEnv *env, jobject class_loader, void *context) {
        auto *agent = static_cast<DejavuAgent *>(context);
        const AgentStartupResult startup = agent_start_control_scripts(
            agent->init_script_, agent->early_hook_script_,
            [&](const std::string &script, std::string *error) {
                return agent->hook_manager_.start_control(env, class_loader, script, error);
            },
            [&](const std::string &script, std::string *error) {
                std::string result;
                return agent->hook_manager_.eval_control(script, &result, error);
            });
        if (!startup.initialized) {
            LOGE("Lua control initialization failed: %s", startup.error.c_str());
            return;
        }
        if (startup.early_hook_failed) {
            LOGE("early hook failed for %s: %s; live control remains available",
                 agent->process_name_.c_str(), startup.error.c_str());
        }
        LOGD("persistent Lua control initialized");
        agent->start_live_control();
    }

    void start_live_control() {
        if (control_endpoint_.socket_name[0] == '\0') {
            LOGD("live control unavailable: no negotiated endpoint");
            return;
        }
        const int listener = dejavu_control_bind_listener(control_endpoint_);
        if (listener < 0) {
            LOGE("failed to bind Agent control listener: %s", strerror(errno));
            return;
        }
        try {
            std::thread([this, listener]() { run_live_control(listener); }).detach();
        } catch (const std::system_error &error) {
            close(listener);
            LOGE("failed to start RPC control thread: %s", error.what());
        }
    }

    bool serve_rpc(int control) {
        DejavuControlRpcMessage request;
        if (!dejavu_control_read_rpc(control, &request)) {
            return false;
        }
        DejavuControlRpcMessage response = {
            DejavuControlRpcType::kExecuteResponse,
            request.request_id,
            DEJAVU_CONTROL_RPC_PROTOCOL_ERROR,
            "invalid RPC request",
        };
        if (request.type == DejavuControlRpcType::kExecuteRequest &&
            request.payload.find('\0') == std::string::npos) {
            std::string result;
            std::string error;
            if (hook_manager_.eval_control(request.payload, &result, &error)) {
                response.status = DEJAVU_CONTROL_RPC_OK;
                response.payload = std::move(result);
                if (response.payload.size() > kDejavuMaximumControlScriptSize) {
                    /* Never drop the connection because the result is too
                       large; report a regular execution error instead. */
                    LOGE("RPC control result too large: request=%llu bytes=%zu",
                         static_cast<unsigned long long>(request.request_id),
                         response.payload.size());
                    response.status = DEJAVU_CONTROL_RPC_EXECUTION_ERROR;
                    response.payload = "control result exceeds the 1 MiB response limit";
                } else {
                    LOGD("RPC control applied: request=%llu bytes=%zu",
                         static_cast<unsigned long long>(request.request_id),
                         request.payload.size());
                }
            } else {
                response.status = DEJAVU_CONTROL_RPC_EXECUTION_ERROR;
                response.payload = std::move(error);
                LOGE("RPC control failed: request=%llu error=%s",
                     static_cast<unsigned long long>(request.request_id), response.payload.c_str());
            }
        }
        return dejavu_control_write_rpc(control, response,
                                        std::chrono::steady_clock::now() + std::chrono::seconds(2));
    }

    void run_live_control(int listener) {
        struct JniThreadDetachGuard {
            JavaVM *vm;
            ~JniThreadDetachGuard() {
                if (vm == nullptr) {
                    return;
                }
                JNIEnv *attached = nullptr;
                if (vm->GetEnv(reinterpret_cast<void **>(&attached), JNI_VERSION_1_6) == JNI_OK) {
                    vm->DetachCurrentThread();
                }
            }
        } detach_guard{vm_};

        LOGD("Agent control listener ready");
        for (;;) {
            pollfd descriptor = {listener, POLLIN, 0};
            const int status = poll(&descriptor, 1, -1);
            if (status < 0 && errno == EINTR) {
                continue;
            }
            if (status < 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                break;
            }
            const int control = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
            if (control < 0) {
                const int err = errno;
                if (err == EINTR || err == EAGAIN || err == EWOULDBLOCK || err == ECONNABORTED ||
                    err == EPROTO) {
                    continue;
                }
                if (err == EMFILE || err == ENFILE || err == ENOBUFS || err == ENOMEM) {
                    LOGE("Agent control accept4 transient error: %s", strerror(err));
                    poll(nullptr, 0, 50);
                    continue;
                }
                LOGE("Agent control accept4 fatal error: %s", strerror(err));
                break;
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            if (dejavu_control_peer_is_root(control) &&
                dejavu_control_read_authentication(control, control_endpoint_, deadline) &&
                dejavu_control_write_frame(control, nullptr, 0, deadline)) {
                LOGD("Agent RPC control authenticated root companion");
                while (serve_rpc(control)) {
                }
                LOGD("Agent RPC control disconnected");
            }
            close(control);
        }
        close(listener);
        LOGE("Agent control listener stopped");
    }
};

void *create_agent(const DejavuAgentCreateArgs *args) {
    if (args == nullptr || args->struct_size < sizeof(DejavuAgentCreateArgs) ||
        args->abi_version != DEJAVU_AGENT_ABI_VERSION || args->env == nullptr ||
        args->vm == nullptr || args->module_dir_fd < 0 || args->process_name == nullptr) {
        return nullptr;
    }
    auto *agent = new (std::nothrow) DejavuAgent(*args);
    if (agent == nullptr || !agent->is_ready()) {
        delete agent;
        return nullptr;
    }
    return agent;
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

} // namespace

extern "C" __attribute__((visibility("default"))) const DejavuAgentApi *
dejavu_agent_get_api(uint32_t abi_version) {
    return abi_version == DEJAVU_AGENT_ABI_VERSION ? &kAgentApi : nullptr;
}
