#include "hook_manager_internal.h"

#include <android/log.h>

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#include "hook_compile_diagnostics.h"
#include "hook_api_runtime.h"

namespace {

constexpr char kLogTag[] = "DejavuHook";
#if DEJAVU_RUN_DEVICE_STRESS
constexpr char kStressClassName[] = "io.dejavu.bridge.HookStress";
#endif

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, kLogTag, __VA_ARGS__)

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

int HookManager::Impl::lua_install(
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
    std::string source(dejavu_hook_api_preamble());
    source.append(value_string(arguments[3]));
    const int status = manager->runtime->engine_compile(
        manager->engine,
        source.c_str(),
        &module,
        compile_error,
        sizeof(compile_error));
    if (status != DEJAVU_OK) {
        copy_error(
            error,
            error_size,
            dejavu_hook_format_compile_error(compile_error, dejavu_hook_api_preamble()));
        return status;
    }
    void *before_symbol =
        manager->runtime->module_symbol(module, DEJAVU_HOOK_BEFORE_SYMBOL);
    void *after_symbol =
        manager->runtime->module_symbol(module, DEJAVU_HOOK_AFTER_SYMBOL);
    if (before_symbol == nullptr && after_symbol == nullptr) {
        manager->runtime->module_destroy(module);
        copy_error(error, error_size, "C source must define before_hook or after_hook");
        return DEJAVU_ERROR_TCC_SYMBOL;
    }
    DejavuNativeHookCallback before = nullptr;
    DejavuNativeHookCallback after = nullptr;
    static_assert(sizeof(before) == sizeof(before_symbol));
    memcpy(&before, &before_symbol, sizeof(before));
    memcpy(&after, &after_symbol, sizeof(after));

    std::string install_error;
    bool hook_attempted = false;
    Record *record = manager->install(
        manager->current_env(),
        value_string(arguments[0]),
        value_string(arguments[1]),
        value_string(arguments[2]),
        module,
        before,
        after,
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

int HookManager::Impl::lua_list(
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

int HookManager::Impl::lua_remove(
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

int HookManager::Impl::lua_uninstall(
    dejavu_control_session *,
    void *context,
    const dejavu_value *arguments,
    size_t argument_count,
    dejavu_value *result,
    char *error,
    size_t error_size) {
    auto *manager = static_cast<Impl *>(context);
    if (argument_count != 1 || arguments[0].type != DEJAVU_VALUE_INTEGER) {
        copy_error(error, error_size, "hook.uninstall expects a hook id");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    Record *record = manager->find(
        static_cast<unsigned long long>(arguments[0].integer_value));
    if (record == nullptr || record->bootstrap ||
        record->retired.load(std::memory_order_acquire)) {
        result->type = DEJAVU_VALUE_BOOLEAN;
        result->boolean_value = false;
        return DEJAVU_OK;
    }
    std::string uninstall_error;
    const bool removed = manager->uninstall_record(record, &uninstall_error);
    if (!removed) {
        copy_error(error, error_size, uninstall_error);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    result->type = DEJAVU_VALUE_BOOLEAN;
    result->boolean_value = true;
    return DEJAVU_OK;
}

int HookManager::Impl::lua_clear(
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
int HookManager::Impl::lua_protocol_test(
    dejavu_control_session *,
    void *context,
    const dejavu_value *arguments,
    size_t argument_count,
    dejavu_value *result,
    char *error,
    size_t error_size) {
    auto *manager = static_cast<Impl *>(context);
    if (argument_count != 1 || arguments[0].type != DEJAVU_VALUE_INTEGER) {
        copy_error(error, error_size, "hook.protocol_test expects a hook id");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    Record *record = manager->find(
        static_cast<unsigned long long>(arguments[0].integer_value));
    if (record == nullptr || record->class_name != kStressClassName ||
        record->method_name != "protocolTarget") {
        copy_error(error, error_size, "invalid hook.protocol_test target");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    JNIEnv *env = manager->current_env();
    jmethodID run_test = manager->stress_class == nullptr
        ? nullptr
        : env->GetStaticMethodID(manager->stress_class, "runProtocolTest", "()I");
    if (env->ExceptionCheck() || run_test == nullptr) {
        env->ExceptionClear();
        copy_error(error, error_size, "unable to resolve HookStress.runProtocolTest");
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    const jint failures = env->CallStaticIntMethod(manager->stress_class, run_test);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        copy_error(error, error_size, "HookStress.runProtocolTest raised an exception");
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    result->type = DEJAVU_VALUE_BOOLEAN;
    result->boolean_value = failures == 0;
    return DEJAVU_OK;
}

int HookManager::Impl::lua_helper_test(
    dejavu_control_session *,
    void *context,
    const dejavu_value *arguments,
    size_t argument_count,
    dejavu_value *result,
    char *error,
    size_t error_size) {
    auto *manager = static_cast<Impl *>(context);
    if (argument_count != 1 || arguments[0].type != DEJAVU_VALUE_INTEGER) {
        copy_error(error, error_size, "hook.helper_test expects a hook id");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    Record *record = manager->find(
        static_cast<unsigned long long>(arguments[0].integer_value));
    if (record == nullptr || record->class_name != kStressClassName) {
        copy_error(error, error_size, "invalid hook.helper_test target");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    JNIEnv *env = manager->current_env();
    jmethodID run_test = manager->stress_class == nullptr
        ? nullptr
        : env->GetStaticMethodID(manager->stress_class, "runHelperTest", "()I");
    if (env->ExceptionCheck() || run_test == nullptr) {
        env->ExceptionClear();
        copy_error(error, error_size, "unable to resolve HookStress.runHelperTest");
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    const jint failures = env->CallStaticIntMethod(manager->stress_class, run_test);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        copy_error(error, error_size, "HookStress.runHelperTest raised an exception");
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    result->type = DEJAVU_VALUE_INTEGER;
    result->integer_value = failures;
    return DEJAVU_OK;
}

int HookManager::Impl::lua_exception_test(
    dejavu_control_session *,
    void *context,
    const dejavu_value *arguments,
    size_t argument_count,
    dejavu_value *result,
    char *error,
    size_t error_size) {
    auto *manager = static_cast<Impl *>(context);
    if (argument_count != 1 || arguments[0].type != DEJAVU_VALUE_INTEGER) {
        copy_error(error, error_size, "hook.exception_test expects a hook id");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    Record *record = manager->find(
        static_cast<unsigned long long>(arguments[0].integer_value));
    if (record == nullptr || record->class_name != kStressClassName ||
        record->method_name != "exceptionTarget") {
        copy_error(error, error_size, "invalid hook.exception_test target");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    JNIEnv *env = manager->current_env();
    jmethodID run_test = manager->stress_class == nullptr
        ? nullptr
        : env->GetStaticMethodID(manager->stress_class, "runExceptionTest", "()I");
    if (env->ExceptionCheck() || run_test == nullptr) {
        env->ExceptionClear();
        copy_error(error, error_size, "unable to resolve HookStress.runExceptionTest");
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    const jint failures = env->CallStaticIntMethod(manager->stress_class, run_test);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        copy_error(error, error_size, "HookStress.runExceptionTest raised an exception");
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    result->type = DEJAVU_VALUE_BOOLEAN;
    result->boolean_value = failures == 0;
    return DEJAVU_OK;
}

int HookManager::Impl::lua_stress(
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

int HookManager::Impl::lua_log(
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
