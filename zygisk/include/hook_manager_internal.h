#pragma once

#include "hook_manager.h"
#include "hook_api_runtime.h"
#include "agent_helpers.h"
#include "plugin_manager.h"
#include "java_invoker.h"
#include "native_hook_manager.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace dejavu_hook_manager_internal {

extern HookManager *g_manager;
extern thread_local bool g_inside_native_hook;

// Stream handles opened through the Lua hook.stream_* API are owned by this
// token and closed when the control session/hook manager is destroyed.
inline constexpr uint64_t kLuaStreamOwnerToken = 0x4C75615F5354524DULL; // Lua_STRM

} // namespace dejavu_hook_manager_internal

struct HookManager::Impl {
    struct Record {
        Impl *manager = nullptr;
        unsigned long long id = 0;
        std::string class_name;
        std::string method_name;
        std::string signature;
        DejavuHookSignature parsed_signature;
        jobject target = nullptr;
        jobject backup = nullptr;
        jobject bridge = nullptr;
        jobject class_loader = nullptr;
        dejavu_module *module = nullptr;
        DejavuNativeHookCallback before = nullptr;
        DejavuNativeHookCallback after = nullptr;
        bool is_static = false;
        std::atomic<bool> active{true};
        std::atomic<bool> ready{false};
        std::atomic<bool> retiring{false};
        std::atomic<bool> retired{false};
        std::atomic<unsigned int> in_flight{0};
        bool bootstrap = false;
        std::atomic<bool> bootstrap_started{false};
        HookManager::BootstrapCallback bootstrap_callback = nullptr;
        void *bootstrap_context = nullptr;
    };

    struct DispatchGuard {
        Record *record;
        explicit DispatchGuard(Record *record) : record(record) {
            record->in_flight.fetch_add(1, std::memory_order_acq_rel);
        }
        ~DispatchGuard() {
            record->in_flight.fetch_sub(1, std::memory_order_release);
        }
    };

    HookManager *owner = nullptr;
    JavaVM *vm = nullptr;
    const DejavuLsplantBackend *backend = nullptr;
    DejavuRuntime *runtime = nullptr;
    dejavu_engine *engine = nullptr;
    dejavu_control_session *session = nullptr;
    std::unique_ptr<NativeHookManager> native_hooks;
    std::unique_ptr<JavaInvoker> java_invoker;
    std::unique_ptr<AgentHelpers> helpers;
    std::unique_ptr<PluginManager> plugins;
    jobject app_class_loader = nullptr;
    jobject bridge_class_loader = nullptr;
    jclass bridge_class = nullptr;
    jmethodID bridge_constructor = nullptr;
    jobject bridge_callback = nullptr;
#if DEJAVU_RUN_DEVICE_STRESS
    jclass stress_class = nullptr;
#endif
    jmethodID bridge_invoke_original = nullptr;
    DejavuHookJniApi hook_jni_api;
    using HookTable = std::vector<Record *>;
    std::mutex writer_mutex;
    std::vector<std::unique_ptr<Record>> owned_records;
    std::shared_ptr<const HookTable> hook_table = std::make_shared<const HookTable>();
    std::atomic<unsigned long long> next_id{1};
#if DEJAVU_RUN_DEVICE_STRESS
    std::atomic<unsigned long long> stress_hook_id{0};
    std::atomic<unsigned long long> stress_calls{0};
    std::atomic<unsigned int> stress_in_flight{0};
    std::atomic<unsigned int> stress_max_parallel{0};
#endif

    Record *find(unsigned long long id);
    void remove_from_table(Record *target);
    bool uninstall_record(Record *record, std::string *error);
    JNIEnv *current_env();
    bool load_bridge(JNIEnv *env, std::string *error);
    jobject load_class(JNIEnv *env, const std::string &name, jobject loader);
    Record *install(JNIEnv *env, const std::string &class_name, const std::string &method_name,
                    const std::string &signature, dejavu_module *module,
                    DejavuNativeHookCallback before, DejavuNativeHookCallback after, jobject loader,
                    HookManager::BootstrapCallback bootstrap_callback, void *bootstrap_context,
                    bool *hook_attempted, std::string *error);

    static int lua_install(dejavu_control_session *, void *, const dejavu_value *, size_t,
                           dejavu_value *, char *, size_t);
    static int lua_list(dejavu_control_session *, void *, const dejavu_value *, size_t,
                        dejavu_value *, char *, size_t);
    static int lua_remove(dejavu_control_session *, void *, const dejavu_value *, size_t,
                          dejavu_value *, char *, size_t);
    static int lua_uninstall(dejavu_control_session *, void *, const dejavu_value *, size_t,
                             dejavu_value *, char *, size_t);
    static int lua_clear(dejavu_control_session *, void *, const dejavu_value *, size_t,
                         dejavu_value *, char *, size_t);
#if DEJAVU_RUN_DEVICE_STRESS
    static int lua_protocol_test(dejavu_control_session *, void *, const dejavu_value *, size_t,
                                 dejavu_value *, char *, size_t);
    static int lua_helper_test(dejavu_control_session *, void *, const dejavu_value *, size_t,
                               dejavu_value *, char *, size_t);
    static int lua_exception_test(dejavu_control_session *, void *, const dejavu_value *, size_t,
                                  dejavu_value *, char *, size_t);
    static int lua_stress(dejavu_control_session *, void *, const dejavu_value *, size_t,
                          dejavu_value *, char *, size_t);
#endif
    static int lua_log(dejavu_control_session *, void *, const dejavu_value *, size_t,
                       dejavu_value *, char *, size_t);
    static int lua_stream_start(dejavu_control_session *, void *, const dejavu_value *, size_t,
                                dejavu_value *, char *, size_t);
    static int lua_stream_acquire(dejavu_control_session *, void *, const dejavu_value *, size_t,
                                  dejavu_value *, char *, size_t);
    static int lua_stream_release(dejavu_control_session *, void *, const dejavu_value *, size_t,
                                  dejavu_value *, char *, size_t);
    static int lua_stream_stop(dejavu_control_session *, void *, const dejavu_value *, size_t,
                               dejavu_value *, char *, size_t);
    static int lua_stream_stats(dejavu_control_session *, void *, const dejavu_value *, size_t,
                                dejavu_value *, char *, size_t);
    static int lua_stream_listen(dejavu_control_session *, void *, const dejavu_value *, size_t,
                                 dejavu_value *, char *, size_t);
    static int lua_stream_accept(dejavu_control_session *, void *, const dejavu_value *, size_t,
                                 dejavu_value *, char *, size_t);
    static int lua_stream_read(dejavu_control_session *, void *, const dejavu_value *, size_t,
                               dejavu_value *, char *, size_t);
    static int lua_stream_write(dejavu_control_session *, void *, const dejavu_value *, size_t,
                                dejavu_value *, char *, size_t);
    static int lua_stream_close(dejavu_control_session *, void *, const dejavu_value *, size_t,
                                dejavu_value *, char *, size_t);
};
