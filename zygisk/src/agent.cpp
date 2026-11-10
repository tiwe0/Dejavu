#include "dejavu_agent_api.h"

#include <android/log.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <new>
#include <string>
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
            LOGD("live control unavailable: socket endpoint not established");
            return;
        }
        std::thread([this]() { run_live_control(); }).detach();
        LOGD("abstract-socket RPC control starting");
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

    void run_live_control() {
        unsigned int retry_delay_ms = 100;
        bool unavailable_logged = false;
        while (true) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            const int control = dejavu_control_connect(control_endpoint_, deadline);
            if (control < 0) {
                if (!unavailable_logged) {
                    LOGE("RPC control unavailable, retrying: %s", strerror(errno));
                    unavailable_logged = true;
                }
                poll(nullptr, 0, static_cast<int>(retry_delay_ms));
                retry_delay_ms =
                    retry_delay_ms < 5000 ? std::min(retry_delay_ms * 2, 5000u) : 5000u;
                continue;
            }
            std::string acknowledgement;
            if (!dejavu_control_peer_is_root(control) ||
                !dejavu_control_write_authentication(control, control_endpoint_, deadline) ||
                !dejavu_control_read_frame(control, &acknowledgement, 0, deadline)) {
                if (!unavailable_logged) {
                    LOGE("RPC control authentication failed, retrying");
                    unavailable_logged = true;
                }
                close(control);
                poll(nullptr, 0, static_cast<int>(retry_delay_ms));
                retry_delay_ms =
                    retry_delay_ms < 5000 ? std::min(retry_delay_ms * 2, 5000u) : 5000u;
                continue;
            }
            retry_delay_ms = 100;
            unavailable_logged = false;
            LOGD("abstract-socket RPC control connected");
            while (serve_rpc(control)) {
            }
            close(control);
            LOGD("abstract-socket RPC control disconnected; reconnecting");
        }
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

} // namespace

extern "C" __attribute__((visibility("default"))) const DejavuAgentApi *
dejavu_agent_get_api(uint32_t abi_version) {
    return abi_version == DEJAVU_AGENT_ABI_VERSION ? &kAgentApi : nullptr;
}
