#include "hook_manager_internal.h"

#include <atomic>
#include <iomanip>
#include <memory>
#include <mutex>
#include <new>
#include <sched.h>
#include <sstream>
#include <utility>
#include <vector>

#include "hook_bridge_dex.h"
#include "hook_api_runtime.h"

namespace {

constexpr char kBridgeClassName[] = "io.dejavu.bridge.HookBridge";
#if DEJAVU_RUN_DEVICE_STRESS
constexpr char kStressClassName[] = "io.dejavu.bridge.HookStress";
#endif

void set_error(std::string *error, const char *message) {
    if (error != nullptr) {
        error->assign(message != nullptr ? message : "unknown error");
    }
}

}  // namespace

namespace dejavu_hook_manager_internal {
HookManager *g_manager = nullptr;
thread_local bool g_inside_native_hook = false;
}

using namespace dejavu_hook_manager_internal;

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

HookManager::Impl::Record *HookManager::Impl::find(unsigned long long id) {
    const auto snapshot = std::atomic_load_explicit(
        &hook_table, std::memory_order_acquire);
    for (Record *record : *snapshot) {
        if (record->id == id) {
            return record;
        }
    }
    return nullptr;
}

void HookManager::Impl::remove_from_table(Record *target) {
    std::lock_guard<std::mutex> lock(writer_mutex);
    const auto current = std::atomic_load_explicit(
        &hook_table, std::memory_order_acquire);
    auto next = std::make_shared<HookTable>();
    next->reserve(current->size());
    for (Record *record : *current) {
        if (record != target) {
            next->push_back(record);
        }
    }
    std::atomic_store_explicit(
        &hook_table,
        std::shared_ptr<const HookTable>(std::move(next)),
        std::memory_order_release);
}

bool HookManager::Impl::uninstall_record(Record *record, std::string *error) {
    if (record == nullptr || record->manager != this || record->bootstrap) {
        set_error(error, "invalid or protected hook");
        return false;
    }
    if (record->retired.load(std::memory_order_acquire)) {
        set_error(error, "hook is already uninstalled");
        return false;
    }
    if (g_inside_native_hook) {
        set_error(error, "hook.uninstall cannot run from a native callback");
        return false;
    }
    bool expected = false;
    if (!record->retiring.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        set_error(error, "hook uninstall is already in progress");
        return false;
    }
    record->active.store(false, std::memory_order_release);
    JNIEnv *env = current_env();
    if (env == nullptr || record->target == nullptr ||
        !backend->unhook(env, record->target) || env->ExceptionCheck()) {
        if (env != nullptr && env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        record->retiring.store(false, std::memory_order_release);
        set_error(error, "LSPlant failed to uninstall hook");
        return false;
    }

    while (record->in_flight.load(std::memory_order_acquire) != 0) {
        sched_yield();
    }

    if (record->module != nullptr) {
        runtime->module_destroy(record->module);
        record->module = nullptr;
    }
    if (env != nullptr) {
        if (record->target != nullptr) {
            env->DeleteGlobalRef(record->target);
            record->target = nullptr;
        }
        if (record->bridge != nullptr) {
            env->DeleteGlobalRef(record->bridge);
            record->bridge = nullptr;
        }
        if (record->class_loader != nullptr) {
            env->DeleteGlobalRef(record->class_loader);
            record->class_loader = nullptr;
        }
    }
    record->before = nullptr;
    record->after = nullptr;
    record->parsed_signature.parameters.clear();
    record->retired.store(true, std::memory_order_release);
    remove_from_table(record);
    return true;
}

JNIEnv *HookManager::Impl::current_env() {
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

bool HookManager::Impl::load_bridge(JNIEnv *env, std::string *error) {
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

jobject HookManager::Impl::load_class(JNIEnv *env, const std::string &name, jobject loader) {
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

HookManager::Impl::Record *HookManager::Impl::install(
    JNIEnv *env,
    const std::string &class_name,
    const std::string &method_name,
    const std::string &signature,
    dejavu_module *module,
    DejavuNativeHookCallback before,
    DejavuNativeHookCallback after,
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
    DejavuHookSignature parsed_signature;
    if (!dejavu_parse_hook_signature(signature, &parsed_signature, error)) {
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
    record->parsed_signature = std::move(parsed_signature);
    record->module = module;
    record->before = before;
    record->after = after;
    jobject context_loader = loader;
#if DEJAVU_RUN_DEVICE_STRESS
    if (class_name == kStressClassName) {
        context_loader = bridge_class_loader;
    }
#endif
    record->class_loader = context_loader == nullptr
        ? nullptr
        : env->NewGlobalRef(context_loader);
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
    if (record->target == nullptr || record->bridge == nullptr ||
        (context_loader != nullptr && record->class_loader == nullptr)) {
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
    if (!dejavu_hook_api_initialize(env, &impl_->hook_jni_api, error)) {
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
    if (!dejavu_hook_api_register_symbols(
            impl_->runtime, impl_->engine, error)) {
        return false;
    }
    int status = DEJAVU_OK;

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
        {"uninstall", Impl::lua_uninstall},
        {"clear", Impl::lua_clear},
        {"log", Impl::lua_log},
#if DEJAVU_RUN_DEVICE_STRESS
        {"protocol_test", Impl::lua_protocol_test},
        {"helper_test", Impl::lua_helper_test},
        {"exception_test", Impl::lua_exception_test},
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

bool HookManager::eval_control(
    const std::string &script,
    std::string *result,
    std::string *error) {
    if (impl_ == nullptr || impl_->session == nullptr) {
        set_error(error, "Lua control is not initialized");
        return false;
    }
    char runtime_error[512];
    dejavu_value value = {};
    const int status = impl_->runtime->control_session_eval(
        impl_->session, script.c_str(), &value, runtime_error, sizeof(runtime_error));
    if (status != DEJAVU_OK) {
        impl_->runtime->value_release(&value);
        set_error(error, runtime_error);
        return false;
    }
    std::ostringstream output;
    switch (value.type) {
        case DEJAVU_VALUE_NIL:
            output << "nil";
            break;
        case DEJAVU_VALUE_BOOLEAN:
            output << (value.boolean_value ? "true" : "false");
            break;
        case DEJAVU_VALUE_INTEGER:
            output << value.integer_value;
            break;
        case DEJAVU_VALUE_NUMBER:
            output << std::setprecision(17) << value.number_value;
            break;
        case DEJAVU_VALUE_STRING:
            if (value.string_size != 0 && value.string_value == nullptr) {
                impl_->runtime->value_release(&value);
                set_error(error, "invalid Lua string result");
                return false;
            }
            if (value.string_size != 0) {
                output.write(value.string_value, static_cast<std::streamsize>(value.string_size));
            }
            break;
        default:
            impl_->runtime->value_release(&value);
            set_error(error, "unsupported Lua RPC result type");
            return false;
    }
    impl_->runtime->value_release(&value);
    if (result != nullptr) {
        *result = output.str();
    }
    return true;
}
