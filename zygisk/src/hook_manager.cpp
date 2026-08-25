#include "hook_manager.h"

#include <android/log.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <sched.h>
#include <sstream>
#include <utility>
#include <vector>

#include "hook_bridge_dex.h"

namespace {

constexpr char kLogTag[] = "DejavuHook";
constexpr char kBridgeClassName[] = "io.dejavu.bridge.HookBridge";
#if DEJAVU_RUN_DEVICE_STRESS
constexpr char kStressClassName[] = "io.dejavu.bridge.HookStress";
#endif

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, kLogTag, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)

HookManager *g_manager = nullptr;
thread_local bool g_inside_native_hook = false;

void set_error(std::string *error, const char *message) {
    if (error != nullptr) {
        error->assign(message != nullptr ? message : "unknown error");
    }
}

void copy_error(char *error, size_t error_size, const std::string &message) {
    if (error != nullptr && error_size != 0) {
        snprintf(error, error_size, "%s", message.c_str());
    }
}

std::string value_string(const dejavu_value &value) {
    return value.string_value == nullptr
        ? std::string()
        : std::string(value.string_value, value.string_size);
}

bool set_owned_string(dejavu_value *value, const std::string &text) {
    char *copy = static_cast<char *>(malloc(text.size() + 1));
    if (copy == nullptr) {
        return false;
    }
    memcpy(copy, text.data(), text.size());
    copy[text.size()] = '\0';
    value->type = DEJAVU_VALUE_STRING;
    value->owned = 1;
    value->string_value = copy;
    value->string_size = text.size();
    return true;
}

}  // namespace

struct HookManager::Impl {
    struct Record {
        Impl *manager = nullptr;
        unsigned long long id = 0;
        std::string class_name;
        std::string method_name;
        std::string signature;
        jobject target = nullptr;
        jobject backup = nullptr;
        jobject bridge = nullptr;
        dejavu_module *module = nullptr;
        DejavuNativeHookCallback callback = nullptr;
        bool is_static = false;
        std::atomic<bool> active{true};
        std::atomic<bool> ready{false};
        bool bootstrap = false;
        std::atomic<bool> bootstrap_started{false};
        BootstrapCallback bootstrap_callback = nullptr;
        void *bootstrap_context = nullptr;
    };

    HookManager *owner = nullptr;
    JavaVM *vm = nullptr;
    const DejavuLsplantBackend *backend = nullptr;
    DejavuRuntime *runtime = nullptr;
    dejavu_engine *engine = nullptr;
    dejavu_control_session *session = nullptr;
    jobject app_class_loader = nullptr;
    jobject bridge_class_loader = nullptr;
    jclass bridge_class = nullptr;
    jmethodID bridge_constructor = nullptr;
    jobject bridge_callback = nullptr;
#if DEJAVU_RUN_DEVICE_STRESS
    jclass stress_class = nullptr;
#endif
    jclass method_class = nullptr;
    jmethodID method_invoke = nullptr;
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

    Record *find(unsigned long long id) {
        const auto snapshot = std::atomic_load_explicit(
            &hook_table, std::memory_order_acquire);
        for (Record *record : *snapshot) {
            if (record->id == id) {
                return record;
            }
        }
        return nullptr;
    }

    JNIEnv *current_env() {
        JNIEnv *env = nullptr;
        const jint status = vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
        if (status == JNI_OK) {
            return env;
        }
        if (status == JNI_EDETACHED && vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            return env;
        }
        return nullptr;
    }

    bool load_bridge(JNIEnv *env, std::string *error) {
        jclass byte_buffer_class = env->FindClass("java/nio/ByteBuffer");
        jclass dex_loader_class = env->FindClass("dalvik/system/InMemoryDexClassLoader");
        jclass class_loader_class = env->FindClass("java/lang/ClassLoader");
        if (byte_buffer_class == nullptr || dex_loader_class == nullptr ||
            class_loader_class == nullptr) {
            env->ExceptionClear();
            set_error(error, "required bridge loader classes are unavailable");
            return false;
        }
        const jmethodID wrap = env->GetStaticMethodID(
            byte_buffer_class, "wrap", "([B)Ljava/nio/ByteBuffer;");
        const jmethodID get_system = env->GetStaticMethodID(
            class_loader_class, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
        const jmethodID loader_constructor = env->GetMethodID(
            dex_loader_class,
            "<init>",
            "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
        const jmethodID load_class = env->GetMethodID(
            class_loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
        if (wrap == nullptr || get_system == nullptr || loader_constructor == nullptr ||
            load_class == nullptr) {
            env->ExceptionClear();
            set_error(error, "unable to resolve bridge loader methods");
            return false;
        }

        jbyteArray bytes = env->NewByteArray(static_cast<jsize>(dejavu_hook_bridge_dex_len));
        if (bytes == nullptr) {
            set_error(error, "unable to allocate bridge DEX array");
            return false;
        }
        env->SetByteArrayRegion(
            bytes,
            0,
            static_cast<jsize>(dejavu_hook_bridge_dex_len),
            reinterpret_cast<const jbyte *>(dejavu_hook_bridge_dex));
        jobject buffer = env->CallStaticObjectMethod(byte_buffer_class, wrap, bytes);
        jobject parent = env->CallStaticObjectMethod(class_loader_class, get_system);
        jobject loader = env->NewObject(dex_loader_class, loader_constructor, buffer, parent);
        jstring bridge_name = env->NewStringUTF(kBridgeClassName);
        jobject loaded_class = loader == nullptr
            ? nullptr
            : env->CallObjectMethod(loader, load_class, bridge_name);
        if (env->ExceptionCheck() || loaded_class == nullptr) {
            env->ExceptionClear();
            set_error(error, "unable to load HookBridge DEX");
            return false;
        }
#if DEJAVU_RUN_DEVICE_STRESS
        jstring stress_name = env->NewStringUTF(kStressClassName);
        jobject loaded_stress_class = env->CallObjectMethod(loader, load_class, stress_name);
        if (env->ExceptionCheck() || loaded_stress_class == nullptr) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            set_error(error, "unable to load HookStress DEX");
            return false;
        }
        stress_class = static_cast<jclass>(env->NewGlobalRef(loaded_stress_class));
        if (stress_class == nullptr) {
            set_error(error, "unable to retain HookStress class");
            return false;
        }
#endif

        bridge_class_loader = env->NewGlobalRef(loader);
        bridge_class = static_cast<jclass>(env->NewGlobalRef(loaded_class));
        bridge_constructor = env->GetMethodID(bridge_class, "<init>", "(J)V");
        const jmethodID callback = env->GetMethodID(
            bridge_class, "dispatch", "([Ljava/lang/Object;)Ljava/lang/Object;");
        if (bridge_class_loader == nullptr || bridge_class == nullptr ||
            bridge_constructor == nullptr || callback == nullptr) {
            env->ExceptionClear();
            set_error(error, "unable to resolve HookBridge members");
            return false;
        }
        bridge_callback = env->NewGlobalRef(env->ToReflectedMethod(bridge_class, callback, JNI_FALSE));
        if (bridge_callback == nullptr) {
            set_error(error, "unable to reflect HookBridge callback");
            return false;
        }
        return true;
    }

    jobject load_class(JNIEnv *env, const std::string &name, jobject loader) {
        if (loader == nullptr) {
            std::string path = name;
            for (char &character : path) {
                if (character == '.') {
                    character = '/';
                }
            }
            return env->FindClass(path.c_str());
        }
        jclass loader_class = env->FindClass("java/lang/ClassLoader");
        jmethodID load = env->GetMethodID(
            loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
        jstring class_name = env->NewStringUTF(name.c_str());
        return env->CallObjectMethod(loader, load, class_name);
    }

    Record *install(
        JNIEnv *env,
        const std::string &class_name,
        const std::string &method_name,
        const std::string &signature,
        dejavu_module *module,
        DejavuNativeHookCallback callback,
        jobject loader,
        BootstrapCallback bootstrap_callback,
        void *bootstrap_context,
        bool *hook_attempted,
        std::string *error) {
        *hook_attempted = false;
        if (env == nullptr) {
            set_error(error, "hook thread is not attached to ART");
            return nullptr;
        }
        jobject loaded_class = nullptr;
#if DEJAVU_RUN_DEVICE_STRESS
        if (class_name == kStressClassName) {
            loaded_class = stress_class;
        } else
#endif
        {
            loaded_class = load_class(env, class_name, loader);
        }
        if (env->ExceptionCheck() || loaded_class == nullptr) {
            env->ExceptionClear();
            set_error(error, "target class not found");
            return nullptr;
        }
        jclass target_class = static_cast<jclass>(loaded_class);
        bool is_static = false;
        jmethodID target_id = env->GetMethodID(
            target_class, method_name.c_str(), signature.c_str());
        if (target_id == nullptr) {
            env->ExceptionClear();
            target_id = env->GetStaticMethodID(
                target_class, method_name.c_str(), signature.c_str());
            is_static = true;
        }
        if (target_id == nullptr) {
            env->ExceptionClear();
            set_error(error, "target method and JNI signature not found");
            return nullptr;
        }

        auto record = std::make_unique<Record>();
        record->manager = this;
        record->id = next_id.fetch_add(1);
        record->class_name = class_name;
        record->method_name = method_name;
        record->signature = signature;
        record->module = module;
        record->callback = callback;
        record->is_static = is_static;
        record->bootstrap = bootstrap_callback != nullptr;
        record->bootstrap_callback = bootstrap_callback;
        record->bootstrap_context = bootstrap_context;
        jobject target = env->ToReflectedMethod(
            target_class, target_id, is_static ? JNI_TRUE : JNI_FALSE);
        jobject bridge = env->NewObject(
            bridge_class,
            bridge_constructor,
            static_cast<jlong>(reinterpret_cast<uintptr_t>(record.get())));
        record->target = env->NewGlobalRef(target);
        record->bridge = env->NewGlobalRef(bridge);
        if (record->target == nullptr || record->bridge == nullptr) {
            set_error(error, "unable to retain target hook references");
            return nullptr;
        }
        *hook_attempted = true;
        jobject backup = backend->hook(env, target, bridge, bridge_callback);
        if (env->ExceptionCheck() || backup == nullptr) {
            env->ExceptionClear();
            record->ready.store(true, std::memory_order_release);
            std::lock_guard<std::mutex> lock(writer_mutex);
            owned_records.push_back(std::move(record));
            set_error(error, "LSPlant failed to install target hook");
            return nullptr;
        }
        record->backup = env->NewGlobalRef(backup);
        if (record->backup == nullptr) {
            record->active.store(false, std::memory_order_release);
            record->ready.store(true, std::memory_order_release);
            std::lock_guard<std::mutex> lock(writer_mutex);
            owned_records.push_back(std::move(record));
            set_error(error, "unable to retain LSPlant backup method");
            return nullptr;
        }
        record->ready.store(true, std::memory_order_release);
        Record *installed = record.get();
        std::lock_guard<std::mutex> lock(writer_mutex);
        const auto current = std::atomic_load_explicit(
            &hook_table, std::memory_order_acquire);
        auto next = std::make_shared<HookTable>(*current);
        next->push_back(installed);
        owned_records.push_back(std::move(record));
        std::atomic_store_explicit(
            &hook_table,
            std::shared_ptr<const HookTable>(std::move(next)),
            std::memory_order_release);
        return installed;
    }

    static int lua_install(
        dejavu_control_session *,
        void *context,
        const dejavu_value *arguments,
        size_t argument_count,
        dejavu_value *result,
        char *error,
        size_t error_size) {
        auto *manager = static_cast<Impl *>(context);
        if (argument_count != 4 || arguments[0].type != DEJAVU_VALUE_STRING ||
            arguments[1].type != DEJAVU_VALUE_STRING ||
            arguments[2].type != DEJAVU_VALUE_STRING ||
            arguments[3].type != DEJAVU_VALUE_STRING) {
            copy_error(
                error,
                error_size,
                "hook.install expects class, method, JNI signature, C source");
            return DEJAVU_ERROR_INVALID_ARGUMENT;
        }

        dejavu_module *module = nullptr;
        char compile_error[1024];
        const int status = manager->runtime->engine_compile(
            manager->engine,
            value_string(arguments[3]).c_str(),
            &module,
            compile_error,
            sizeof(compile_error));
        if (status != DEJAVU_OK) {
            copy_error(error, error_size, compile_error);
            return status;
        }
        void *symbol = manager->runtime->module_symbol(module, DEJAVU_HOOK_ENTRY_SYMBOL);
        if (symbol == nullptr) {
            manager->runtime->module_destroy(module);
            copy_error(error, error_size, "C source must define dejavu_hook_callback");
            return DEJAVU_ERROR_TCC_SYMBOL;
        }
        DejavuNativeHookCallback callback = nullptr;
        static_assert(sizeof(callback) == sizeof(symbol));
        memcpy(&callback, &symbol, sizeof(callback));

        std::string install_error;
        bool hook_attempted = false;
        Record *record = manager->install(
            manager->current_env(),
            value_string(arguments[0]),
            value_string(arguments[1]),
            value_string(arguments[2]),
            module,
            callback,
            manager->app_class_loader,
            nullptr,
            nullptr,
            &hook_attempted,
            &install_error);
        if (record == nullptr) {
            if (!hook_attempted) {
                manager->runtime->module_destroy(module);
            }
            copy_error(error, error_size, install_error);
            return DEJAVU_ERROR_LUA_EXECUTE;
        }
        result->type = DEJAVU_VALUE_INTEGER;
        result->integer_value = static_cast<long long>(record->id);
        return DEJAVU_OK;
    }

    static int lua_list(
        dejavu_control_session *,
        void *context,
        const dejavu_value *,
        size_t argument_count,
        dejavu_value *result,
        char *error,
        size_t error_size) {
        auto *manager = static_cast<Impl *>(context);
        if (argument_count != 0) {
            copy_error(error, error_size, "hook.list takes no arguments");
            return DEJAVU_ERROR_INVALID_ARGUMENT;
        }
        std::ostringstream output;
        const auto snapshot = std::atomic_load_explicit(
            &manager->hook_table, std::memory_order_acquire);
        for (Record *record : *snapshot) {
            if (!record->bootstrap) {
                output << record->id << '\t'
                       << (record->active.load(std::memory_order_acquire) ? "active" : "disabled")
                       << '\t' << record->class_name << '#' << record->method_name
                       << record->signature << '\n';
            }
        }
        if (!set_owned_string(result, output.str())) {
            copy_error(error, error_size, "unable to allocate hook list");
            return DEJAVU_ERROR_OUT_OF_MEMORY;
        }
        return DEJAVU_OK;
    }

    static int lua_remove(
        dejavu_control_session *,
        void *context,
        const dejavu_value *arguments,
        size_t argument_count,
        dejavu_value *result,
        char *error,
        size_t error_size) {
        auto *manager = static_cast<Impl *>(context);
        if (argument_count != 1 || arguments[0].type != DEJAVU_VALUE_INTEGER) {
            copy_error(error, error_size, "hook.remove expects a hook id");
            return DEJAVU_ERROR_INVALID_ARGUMENT;
        }
        Record *record = manager->find(
            static_cast<unsigned long long>(arguments[0].integer_value));
        bool removed = false;
        if (record != nullptr && !record->bootstrap) {
            removed = record->active.exchange(false, std::memory_order_acq_rel);
        }
        result->type = DEJAVU_VALUE_BOOLEAN;
        result->boolean_value = removed;
        return DEJAVU_OK;
    }

    static int lua_clear(
        dejavu_control_session *,
        void *context,
        const dejavu_value *,
        size_t argument_count,
        dejavu_value *result,
        char *error,
        size_t error_size) {
        auto *manager = static_cast<Impl *>(context);
        if (argument_count != 0) {
            copy_error(error, error_size, "hook.clear takes no arguments");
            return DEJAVU_ERROR_INVALID_ARGUMENT;
        }
        long long count = 0;
        const auto snapshot = std::atomic_load_explicit(
            &manager->hook_table, std::memory_order_acquire);
        for (Record *record : *snapshot) {
            if (!record->bootstrap &&
                record->active.exchange(false, std::memory_order_acq_rel)) {
                ++count;
            }
        }
        result->type = DEJAVU_VALUE_INTEGER;
        result->integer_value = count;
        return DEJAVU_OK;
    }

#if DEJAVU_RUN_DEVICE_STRESS
    static int lua_stress(
        dejavu_control_session *,
        void *context,
        const dejavu_value *arguments,
        size_t argument_count,
        dejavu_value *result,
        char *error,
        size_t error_size) {
        auto *manager = static_cast<Impl *>(context);
        if (argument_count != 4 || arguments[0].type != DEJAVU_VALUE_INTEGER ||
            arguments[1].type != DEJAVU_VALUE_INTEGER ||
            arguments[2].type != DEJAVU_VALUE_INTEGER ||
            arguments[3].type != DEJAVU_VALUE_BOOLEAN) {
            copy_error(error, error_size, "hook.stress expects id, threads, iterations, active");
            return DEJAVU_ERROR_INVALID_ARGUMENT;
        }
        const auto hook_id = static_cast<unsigned long long>(arguments[0].integer_value);
        const int thread_count = static_cast<int>(arguments[1].integer_value);
        const int iterations = static_cast<int>(arguments[2].integer_value);
        const bool expect_active = arguments[3].boolean_value;
        Record *record = manager->find(hook_id);
        if (record == nullptr || record->class_name != kStressClassName ||
            record->method_name != "stressTarget" || thread_count < 2 || thread_count > 64 ||
            iterations < 1 || iterations > 1000000) {
            copy_error(error, error_size, "invalid hook.stress target or workload");
            return DEJAVU_ERROR_INVALID_ARGUMENT;
        }

        JNIEnv *env = manager->current_env();
        jmethodID run_stress = manager->stress_class == nullptr
            ? nullptr
            : env->GetStaticMethodID(manager->stress_class, "runStress", "(II)I");
        if (env->ExceptionCheck() || run_stress == nullptr) {
            env->ExceptionClear();
            copy_error(error, error_size, "unable to resolve HookStress.runStress");
            return DEJAVU_ERROR_LUA_EXECUTE;
        }

        manager->stress_calls.store(0, std::memory_order_release);
        manager->stress_in_flight.store(0, std::memory_order_release);
        manager->stress_max_parallel.store(0, std::memory_order_release);
        manager->stress_hook_id.store(hook_id, std::memory_order_release);
        const jint java_failures = env->CallStaticIntMethod(
            manager->stress_class, run_stress, thread_count, iterations);
        manager->stress_hook_id.store(0, std::memory_order_release);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            copy_error(error, error_size, "HookStress.runStress raised an exception");
            return DEJAVU_ERROR_LUA_EXECUTE;
        }

        const unsigned long long calls = manager->stress_calls.load(std::memory_order_acquire);
        const unsigned int max_parallel =
            manager->stress_max_parallel.load(std::memory_order_acquire);
        const unsigned long long expected_calls =
            static_cast<unsigned long long>(thread_count) * static_cast<unsigned long long>(iterations);
        const bool passed = java_failures == 0 &&
            (expect_active
                 ? calls == expected_calls && max_parallel >= 2
                 : calls == 0 && max_parallel == 0);
        LOGD(
            "stress %s: calls=%llu expected=%llu max_parallel=%u java_failures=%d %s",
            expect_active ? "active" : "disabled",
            calls,
            expect_active ? expected_calls : 0,
            max_parallel,
            java_failures,
            passed ? "PASS" : "FAIL");
        result->type = DEJAVU_VALUE_BOOLEAN;
        result->boolean_value = passed;
        return DEJAVU_OK;
    }
#endif

    static int lua_log(
        dejavu_control_session *,
        void *,
        const dejavu_value *arguments,
        size_t argument_count,
        dejavu_value *result,
        char *error,
        size_t error_size) {
        if (argument_count != 1 || arguments[0].type != DEJAVU_VALUE_STRING) {
            copy_error(error, error_size, "hook.log expects one string");
            return DEJAVU_ERROR_INVALID_ARGUMENT;
        }
        LOGD("%.*s", static_cast<int>(arguments[0].string_size), arguments[0].string_value);
        result->type = DEJAVU_VALUE_NIL;
        return DEJAVU_OK;
    }
};

namespace {

jobject native_dispatch(JNIEnv *env, jobject bridge, jobjectArray arguments) {
    if (g_manager == nullptr) {
        return nullptr;
    }
    jclass bridge_class = env->GetObjectClass(bridge);
    jfieldID token_field = env->GetFieldID(bridge_class, "token", "J");
    const jlong token = env->GetLongField(bridge, token_field);
    return g_manager->dispatch(env, static_cast<unsigned long long>(token), arguments);
}

}  // namespace

extern "C" void *dejavu_hook_call_original(
    void *jni_env,
    void *backup_method,
    void *arguments,
    int is_static) {
    if (g_manager == nullptr || jni_env == nullptr || backup_method == nullptr) {
        return nullptr;
    }
    return g_manager->call_original(
        static_cast<JNIEnv *>(jni_env),
        static_cast<jobject>(backup_method),
        static_cast<jobjectArray>(arguments),
        is_static != 0);
}

extern "C" void dejavu_hook_log(const char *message) {
    LOGD("%s", message != nullptr ? message : "");
}

HookManager::HookManager() : impl_(new (std::nothrow) Impl()) {
    if (impl_ != nullptr) {
        impl_->owner = this;
    }
}

HookManager::~HookManager() {
    delete impl_;
}

bool HookManager::initialize(
    JNIEnv *env,
    JavaVM *vm,
    const DejavuLsplantBackend *backend,
    DejavuRuntime *runtime,
    std::string *error) {
    if (impl_ == nullptr || env == nullptr || vm == nullptr || backend == nullptr ||
        backend->abi_version != 1 || runtime == nullptr) {
        set_error(error, "invalid HookManager initialization argument");
        return false;
    }
    impl_->vm = vm;
    impl_->backend = backend;
    impl_->runtime = runtime;
    if (!impl_->load_bridge(env, error)) {
        return false;
    }
    JNINativeMethod method = {
        const_cast<char *>("dispatch"),
        const_cast<char *>("([Ljava/lang/Object;)Ljava/lang/Object;"),
        reinterpret_cast<void *>(native_dispatch),
    };
    if (env->RegisterNatives(impl_->bridge_class, &method, 1) != JNI_OK) {
        env->ExceptionClear();
        set_error(error, "unable to register HookBridge native callback");
        return false;
    }
    jclass local_method_class = env->FindClass("java/lang/reflect/Method");
    impl_->method_class = static_cast<jclass>(env->NewGlobalRef(local_method_class));
    impl_->method_invoke = env->GetMethodID(
        impl_->method_class,
        "invoke",
        "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;");
    if (impl_->method_class == nullptr || impl_->method_invoke == nullptr) {
        set_error(error, "unable to resolve java.lang.reflect.Method.invoke");
        return false;
    }
    g_manager = this;
    return true;
}

bool HookManager::install_app_bootstrap(
    JNIEnv *env,
    BootstrapCallback callback,
    void *context,
    std::string *error) {
    if (impl_ == nullptr || callback == nullptr) {
        set_error(error, "invalid app bootstrap callback");
        return false;
    }
    bool hook_attempted = false;
    Impl::Record *record = impl_->install(
        env,
        "android.app.ActivityThread",
        "handleBindApplication",
        "(Landroid/app/ActivityThread$AppBindData;)V",
        nullptr,
        nullptr,
        nullptr,
        callback,
        context,
        &hook_attempted,
        error);
    if (record == nullptr) {
        return false;
    }
    return true;
}

bool HookManager::start_control(
    JNIEnv *env,
    jobject app_class_loader,
    const std::string &script,
    std::string *error) {
    if (impl_ == nullptr || env == nullptr || app_class_loader == nullptr ||
        impl_->session != nullptr) {
        set_error(error, "invalid or repeated Lua control initialization");
        return false;
    }
    impl_->app_class_loader = env->NewGlobalRef(app_class_loader);
    impl_->engine = impl_->runtime->engine_create();
    if (impl_->engine == nullptr) {
        set_error(error, "unable to create Dejavu engine");
        return false;
    }
    int status = impl_->runtime->engine_add_symbol(
        impl_->engine,
        "dejavu_hook_call_original",
        reinterpret_cast<const void *>(dejavu_hook_call_original));
    if (status == DEJAVU_OK) {
        status = impl_->runtime->engine_add_symbol(
            impl_->engine,
            "dejavu_hook_log",
            reinterpret_cast<const void *>(dejavu_hook_log));
    }
    if (status != DEJAVU_OK) {
        set_error(error, "unable to register native hook helper symbols");
        return false;
    }

    char runtime_error[512];
    status = impl_->runtime->control_session_create(
        impl_->engine, &impl_->session, runtime_error, sizeof(runtime_error));
    if (status != DEJAVU_OK) {
        set_error(error, runtime_error);
        return false;
    }
    struct Registration {
        const char *name;
        dejavu_host_function function;
    };
    const Registration registrations[] = {
        {"install", Impl::lua_install},
        {"list", Impl::lua_list},
        {"remove", Impl::lua_remove},
        {"clear", Impl::lua_clear},
        {"log", Impl::lua_log},
#if DEJAVU_RUN_DEVICE_STRESS
        {"stress", Impl::lua_stress},
#endif
    };
    for (const Registration &registration : registrations) {
        status = impl_->runtime->control_session_register(
            impl_->session,
            "hook",
            registration.name,
            registration.function,
            impl_,
            runtime_error,
            sizeof(runtime_error));
        if (status != DEJAVU_OK) {
            set_error(error, runtime_error);
            return false;
        }
    }
    dejavu_value result = {};
    status = impl_->runtime->control_session_eval(
        impl_->session, script.c_str(), &result, runtime_error, sizeof(runtime_error));
    impl_->runtime->value_release(&result);
    if (status != DEJAVU_OK) {
        set_error(error, runtime_error);
        return false;
    }
    return true;
}

bool HookManager::eval_control(const std::string &script, std::string *error) {
    if (impl_ == nullptr || impl_->session == nullptr) {
        set_error(error, "Lua control is not initialized");
        return false;
    }
    char runtime_error[512];
    dejavu_value result = {};
    const int status = impl_->runtime->control_session_eval(
        impl_->session, script.c_str(), &result, runtime_error, sizeof(runtime_error));
    impl_->runtime->value_release(&result);
    if (status != DEJAVU_OK) {
        set_error(error, runtime_error);
        return false;
    }
    return true;
}

jobject HookManager::dispatch(
    JNIEnv *env,
    unsigned long long hook_id,
    jobjectArray arguments) {
    if (impl_ == nullptr) {
        return nullptr;
    }
    Impl::Record *record = reinterpret_cast<Impl::Record *>(
        static_cast<uintptr_t>(hook_id));
    if (record == nullptr || record->manager != impl_) {
        return nullptr;
    }
    while (!record->ready.load(std::memory_order_acquire)) {
        sched_yield();
    }
    if (record->bootstrap) {
        jobject result = call_original(env, record->backup, arguments, record->is_static);
        if (env->ExceptionCheck() || record->bootstrap_callback == nullptr ||
            record->bootstrap_started.exchange(true, std::memory_order_acq_rel)) {
            return result;
        }

        LOGD("app bootstrap dispatch entered");
        jclass activity_thread = env->FindClass("android/app/ActivityThread");
        jmethodID current_application = activity_thread == nullptr
            ? nullptr
            : env->GetStaticMethodID(
                  activity_thread,
                  "currentApplication",
                  "()Landroid/app/Application;");
        jobject application = current_application == nullptr
            ? nullptr
            : env->CallStaticObjectMethod(activity_thread, current_application);
        jclass application_class = application == nullptr
            ? nullptr
            : env->GetObjectClass(application);
        jmethodID get_loader = application_class == nullptr
            ? nullptr
            : env->GetMethodID(
                  application_class,
                  "getClassLoader",
                  "()Ljava/lang/ClassLoader;");
        jobject loader = get_loader == nullptr
            ? nullptr
            : env->CallObjectMethod(application, get_loader);
        if (env->ExceptionCheck() || loader == nullptr) {
            env->ExceptionClear();
            LOGE("unable to resolve app ClassLoader after handleBindApplication");
        } else {
            record->bootstrap_callback(env, loader, record->bootstrap_context);
        }
        return result;
    }
    if (!record->active.load(std::memory_order_acquire) ||
        record->callback == nullptr || g_inside_native_hook) {
        return call_original(env, record->backup, arguments, record->is_static);
    }

    g_inside_native_hook = true;
#if DEJAVU_RUN_DEVICE_STRESS
    const bool track_stress =
        impl_->stress_hook_id.load(std::memory_order_acquire) == record->id;
    if (track_stress) {
        impl_->stress_calls.fetch_add(1, std::memory_order_relaxed);
        const unsigned int in_flight =
            impl_->stress_in_flight.fetch_add(1, std::memory_order_acq_rel) + 1;
        unsigned int maximum = impl_->stress_max_parallel.load(std::memory_order_relaxed);
        while (maximum < in_flight &&
               !impl_->stress_max_parallel.compare_exchange_weak(
                   maximum, in_flight, std::memory_order_release, std::memory_order_relaxed)) {
        }
    }
#endif
    void *result = record->callback(
        env,
        record->id,
        record->backup,
        arguments,
        record->is_static ? 1 : 0);
#if DEJAVU_RUN_DEVICE_STRESS
    if (track_stress) {
        impl_->stress_in_flight.fetch_sub(1, std::memory_order_acq_rel);
    }
#endif
    g_inside_native_hook = false;
    return static_cast<jobject>(result);
}

jobject HookManager::call_original(
    JNIEnv *env,
    jobject backup_method,
    jobjectArray arguments,
    bool is_static) {
    if (impl_ == nullptr || env == nullptr || backup_method == nullptr) {
        return nullptr;
    }
    const jsize count = arguments == nullptr ? 0 : env->GetArrayLength(arguments);
    const jsize offset = is_static ? 0 : 1;
    jobject receiver = offset == 0 || count == 0
        ? nullptr
        : env->GetObjectArrayElement(arguments, 0);
    jclass object_class = env->FindClass("java/lang/Object");
    jobjectArray parameters = env->NewObjectArray(
        count >= offset ? count - offset : 0, object_class, nullptr);
    for (jsize index = offset; index < count; ++index) {
        env->SetObjectArrayElement(
            parameters, index - offset, env->GetObjectArrayElement(arguments, index));
    }
    return env->CallObjectMethod(
        backup_method, impl_->method_invoke, receiver, parameters);
}
