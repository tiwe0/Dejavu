#include "native_hook_manager.h"

#include <android/log.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dobby.h>

namespace {

constexpr char kLogTag[] = "DejavuNative";
constexpr char kNativeTargetSymbol[] = "dejavu_native_target";
constexpr char kNativePreamble[] = R"DEJAVU_NATIVE(
extern void *dejavu_native_original_by_id(unsigned long long id);
extern void dejavu_native_hit_by_id(unsigned long long id);
extern void dejavu_native_log(const char *message);
#define dejavu_native_original() dejavu_native_original_by_id(DEJAVU_NATIVE_HOOK_ID)
#define dejavu_native_hit() dejavu_native_hit_by_id(DEJAVU_NATIVE_HOOK_ID)
)DEJAVU_NATIVE";

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, kLogTag, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)

void copy_error(char *error, size_t error_size, const std::string &message) {
    if (error != nullptr && error_size != 0) {
        snprintf(error, error_size, "%s", message.c_str());
    }
}

std::string value_string(const dejavu_value &value) {
    return value.string_value == nullptr ? std::string()
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

int query_protection(void *address, int *protection, std::string *error) {
    FILE *maps = fopen("/proc/self/maps", "re");
    if (maps == nullptr) {
        *error = std::string("unable to open /proc/self/maps: ") + strerror(errno);
        return -1;
    }
    const uintptr_t value = reinterpret_cast<uintptr_t>(address);
    char line[512];
    int result = -1;
    while (fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long start = 0;
        unsigned long end = 0;
        char permissions[8] = {};
        if (sscanf(line, "%lx-%lx %7s", &start, &end, permissions) != 3) {
            continue;
        }
        if (value < start || value >= end) {
            continue;
        }
        int prot = 0;
        if (permissions[0] == 'r') {
            prot |= PROT_READ;
        }
        if (permissions[1] == 'w') {
            prot |= PROT_WRITE;
        }
        if (permissions[2] == 'x') {
            prot |= PROT_EXEC;
        }
        *protection = prot;
        result = 0;
        break;
    }
    fclose(maps);
    if (result != 0) {
        *error = "target address is not mapped";
    }
    return result;
}

bool set_page_protection(void *address, int protection, std::string *error) {
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        *error = "unable to determine the runtime page size";
        return false;
    }
    const uintptr_t value = reinterpret_cast<uintptr_t>(address);
    const uintptr_t page = value - value % static_cast<uintptr_t>(page_size);
    if (mprotect(reinterpret_cast<void *>(page), static_cast<size_t>(page_size), protection) != 0) {
        *error = std::string("mprotect failed: ") + strerror(errno);
        return false;
    }
    return true;
}

void *resolve_target(const std::string &module_name, const std::string &symbol_name,
                     void **handle_out, std::string *error) {
    void *handle = dlopen(module_name.c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (handle == nullptr) {
        const char *detail = dlerror();
        *error = "module is not loaded: " + module_name;
        if (detail != nullptr) {
            *error += std::string(" (") + detail + ")";
        }
        return nullptr;
    }
    dlerror();
    void *symbol = dlsym(handle, symbol_name.c_str());
    const char *detail = dlerror();
    if (detail != nullptr || symbol == nullptr) {
        *error = "symbol not found: " + module_name + "!" + symbol_name;
        if (detail != nullptr) {
            *error += std::string(" (") + detail + ")";
        }
        dlclose(handle);
        return nullptr;
    }
    *handle_out = handle;
    return symbol;
}

} // namespace

extern "C" void *dejavu_native_original_by_id(unsigned long long id);
extern "C" void dejavu_native_hit_by_id(unsigned long long id);
extern "C" void dejavu_native_log(const char *message);

NativeHookManager *NativeHookManager::instance_ = nullptr;

NativeHookManager::NativeHookManager() = default;

NativeHookManager::~NativeHookManager() {
    shutdown();
}

bool NativeHookManager::initialize(DejavuRuntime *runtime, dejavu_engine *engine,
                                   std::string *error) {
    if (runtime == nullptr || engine == nullptr) {
        *error = "invalid native hook manager initialization argument";
        return false;
    }
    struct HelperSymbol {
        const char *name;
        const void *address;
    };
    const HelperSymbol helpers[] = {
        {"dejavu_native_original_by_id",
         reinterpret_cast<const void *>(dejavu_native_original_by_id)},
        {"dejavu_native_hit_by_id", reinterpret_cast<const void *>(dejavu_native_hit_by_id)},
        {"dejavu_native_log", reinterpret_cast<const void *>(dejavu_native_log)},
    };
    for (const HelperSymbol &helper : helpers) {
        if (runtime->engine_add_symbol(engine, helper.name, helper.address) != DEJAVU_OK) {
            *error = std::string("unable to register native hook helper: ") + helper.name;
            return false;
        }
    }
    runtime_ = runtime;
    engine_ = engine;
    instance_ = this;
    return true;
}

bool NativeHookManager::register_lua(dejavu_control_session *session, std::string *error) {
    if (runtime_ == nullptr || session == nullptr) {
        *error = "native hook manager is not initialized";
        return false;
    }
    struct Registration {
        const char *name;
        dejavu_host_function function;
    };
    const Registration registrations[] = {
        {"install", NativeHookManager::lua_install},
        {"uninstall", NativeHookManager::lua_uninstall},
        {"list", NativeHookManager::lua_list},
        {"stats", NativeHookManager::lua_stats},
        {"clear", NativeHookManager::lua_clear},
    };
    char runtime_error[512];
    for (const Registration &registration : registrations) {
        const int status = runtime_->control_session_register(session, "native", registration.name,
                                                              registration.function, this,
                                                              runtime_error, sizeof(runtime_error));
        if (status != DEJAVU_OK) {
            *error = runtime_error;
            return false;
        }
    }
    return true;
}

bool NativeHookManager::install(const std::string &module_name, const std::string &symbol_name,
                                const std::string &source, unsigned long long *id,
                                std::string *error) {
    if (runtime_ == nullptr || engine_ == nullptr) {
        *error = "native hook manager is not initialized";
        return false;
    }
    if (module_name.empty() || symbol_name.empty() || source.empty()) {
        *error = "native.install expects module, symbol and C source";
        return false;
    }
    const unsigned long long hook_id = next_id_.fetch_add(1);
    void *module_handle = nullptr;
    void *target = resolve_target(module_name, symbol_name, &module_handle, error);
    if (target == nullptr) {
        return false;
    }
    int original_protection = 0;
    if (query_protection(target, &original_protection, error) != 0) {
        dlclose(module_handle);
        return false;
    }
    if ((original_protection & PROT_EXEC) == 0) {
        dlclose(module_handle);
        *error = "target symbol is not in an executable mapping";
        return false;
    }

    std::string full_source = "#define DEJAVU_NATIVE_HOOK_ID ";
    full_source += std::to_string(hook_id);
    full_source += "ULL\n";
    full_source += kNativePreamble;
    full_source += source;
    char compile_error[1024] = {};
    dejavu_module *module = nullptr;
    const int compile_status = runtime_->engine_compile(engine_, full_source.c_str(), &module,
                                                        compile_error, sizeof(compile_error));
    if (compile_status != DEJAVU_OK) {
        if (module != nullptr) {
            runtime_->module_destroy(module);
        }
        dlclose(module_handle);
        *error = compile_error[0] != '\0' ? compile_error : "native hook compilation failed";
        return false;
    }
    void *replacement = runtime_->module_symbol(module, kNativeTargetSymbol);
    if (replacement == nullptr) {
        runtime_->module_destroy(module);
        dlclose(module_handle);
        *error = "C source must define dejavu_native_target with the target signature";
        return false;
    }

    auto owned = std::make_unique<Record>();
    Record *record = owned.get();
    record->id = hook_id;
    record->module_name = module_name;
    record->symbol_name = symbol_name;
    record->module_handle = module_handle;
    record->target = target;
    record->module = module;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.emplace(hook_id, std::move(owned));
    }

    if (!set_page_protection(target, original_protection | PROT_WRITE, error)) {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.erase(hook_id);
        runtime_->module_destroy(module);
        dlclose(module_handle);
        return false;
    }
    void *original = nullptr;
    const int hook_status = DobbyHook(target, replacement, &original);
    std::string restore_error;
    const bool restored = set_page_protection(target, original_protection, &restore_error);
    if (hook_status != 0 || !restored) {
        if (hook_status == 0) {
            std::string ignored;
            set_page_protection(target, original_protection | PROT_WRITE, &ignored);
            DobbyDestroy(target);
            set_page_protection(target, original_protection, &ignored);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        records_.erase(hook_id);
        runtime_->module_destroy(module);
        dlclose(module_handle);
        if (hook_status != 0) {
            *error = "DobbyHook failed with status " + std::to_string(hook_status);
        } else {
            *error = "unable to restore the target page protection: " + restore_error;
        }
        return false;
    }
    record->original.store(original, std::memory_order_release);
    record->active.store(true, std::memory_order_release);
    LOGD("native hook %llu installed: %s!%s target=%p original=%p",
         static_cast<unsigned long long>(hook_id), module_name.c_str(), symbol_name.c_str(), target,
         original);
    *id = hook_id;
    return true;
}

bool NativeHookManager::uninstall(unsigned long long id, std::string *error) {
    Record *record = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto iterator = records_.find(id);
        if (iterator == records_.end()) {
            *error = "unknown native hook id: " + std::to_string(id);
            return false;
        }
        record = iterator->second.get();
    }
    if (!record->active.load(std::memory_order_acquire)) {
        *error = "native hook is already uninstalled: " + std::to_string(id);
        return false;
    }
    int protection = 0;
    std::string protection_error;
    const bool have_protection =
        query_protection(record->target, &protection, &protection_error) == 0;
    if (have_protection && !set_page_protection(record->target, protection | PROT_WRITE, error)) {
        return false;
    }
    const int destroy_status = DobbyDestroy(record->target);
    if (have_protection) {
        std::string restore_error;
        set_page_protection(record->target, protection, &restore_error);
    }
    if (destroy_status != 0) {
        *error = "DobbyDestroy failed with status " + std::to_string(destroy_status);
        return false;
    }
    record->active.store(false, std::memory_order_release);
    record->original.store(nullptr, std::memory_order_release);
    if (record->module != nullptr) {
        runtime_->module_destroy(record->module);
        record->module = nullptr;
    }
    if (record->module_handle != nullptr) {
        dlclose(record->module_handle);
        record->module_handle = nullptr;
    }
    LOGD("native hook %llu uninstalled", static_cast<unsigned long long>(id));
    return true;
}

bool NativeHookManager::clear(unsigned long long *removed, std::string *error) {
    std::vector<unsigned long long> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &entry : records_) {
            if (entry.second->active.load(std::memory_order_acquire)) {
                ids.push_back(entry.first);
            }
        }
    }
    unsigned long long count = 0;
    for (const unsigned long long id : ids) {
        std::string uninstall_error;
        if (!uninstall(id, &uninstall_error)) {
            *error = uninstall_error;
            *removed = count;
            return false;
        }
        ++count;
    }
    *removed = count;
    return true;
}

std::string NativeHookManager::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<unsigned long long> ids;
    ids.reserve(records_.size());
    for (const auto &entry : records_) {
        ids.push_back(entry.first);
    }
    std::sort(ids.begin(), ids.end());
    std::string output;
    for (const unsigned long long id : ids) {
        const Record *record = records_.find(id)->second.get();
        if (!output.empty()) {
            output += "\n";
        }
        output += std::to_string(id);
        output += record->active.load(std::memory_order_acquire) ? " active " : " uninstalled ";
        output += record->module_name;
        output += "#";
        output += record->symbol_name;
        output += " hits=";
        output += std::to_string(record->hits.load(std::memory_order_relaxed));
    }
    return output;
}

bool NativeHookManager::stats(unsigned long long id, unsigned long long *hits,
                              std::string *error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = records_.find(id);
    if (iterator == records_.end()) {
        *error = "unknown native hook id: " + std::to_string(id);
        return false;
    }
    *hits = iterator->second->hits.load(std::memory_order_relaxed);
    return true;
}

void NativeHookManager::shutdown() {
    std::vector<unsigned long long> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &entry : records_) {
            if (entry.second->active.load(std::memory_order_acquire)) {
                ids.push_back(entry.first);
            }
        }
    }
    for (const unsigned long long id : ids) {
        std::string ignored;
        uninstall(id, &ignored);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.clear();
    }
    if (instance_ == this) {
        instance_ = nullptr;
    }
    runtime_ = nullptr;
    engine_ = nullptr;
}

void *NativeHookManager::original_for_id(unsigned long long id) {
    struct Cache {
        unsigned long long id = 0;
        void *original = nullptr;
    };
    static thread_local Cache cache;
    if (cache.id == id && cache.original != nullptr) {
        return cache.original;
    }
    NativeHookManager *manager = instance_;
    if (manager == nullptr) {
        return nullptr;
    }
    void *original = nullptr;
    {
        std::lock_guard<std::mutex> lock(manager->mutex_);
        const auto iterator = manager->records_.find(id);
        if (iterator == manager->records_.end()) {
            return nullptr;
        }
        original = iterator->second->original.load(std::memory_order_acquire);
    }
    if (original != nullptr) {
        cache.id = id;
        cache.original = original;
    }
    return original;
}

void NativeHookManager::hit_for_id(unsigned long long id) {
    struct Cache {
        unsigned long long id = 0;
        Record *record = nullptr;
    };
    static thread_local Cache cache;
    if (cache.id != id || cache.record == nullptr) {
        NativeHookManager *manager = instance_;
        if (manager == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(manager->mutex_);
        const auto iterator = manager->records_.find(id);
        if (iterator == manager->records_.end()) {
            return;
        }
        cache.id = id;
        cache.record = iterator->second.get();
    }
    cache.record->hits.fetch_add(1, std::memory_order_relaxed);
}

void NativeHookManager::log_message(const char *message) {
    __android_log_write(ANDROID_LOG_INFO, kLogTag, message != nullptr ? message : "(null)");
}

extern "C" void *dejavu_native_original_by_id(unsigned long long id) {
    return NativeHookManager::original_for_id(id);
}

extern "C" void dejavu_native_hit_by_id(unsigned long long id) {
    NativeHookManager::hit_for_id(id);
}

extern "C" void dejavu_native_log(const char *message) {
    NativeHookManager::log_message(message);
}

int NativeHookManager::lua_install(dejavu_control_session *, void *context,
                                   const dejavu_value *arguments, size_t argument_count,
                                   dejavu_value *result, char *error, size_t error_size) {
    auto *manager = static_cast<NativeHookManager *>(context);
    if (manager == nullptr || argument_count != 3 || arguments[0].type != DEJAVU_VALUE_STRING ||
        arguments[1].type != DEJAVU_VALUE_STRING || arguments[2].type != DEJAVU_VALUE_STRING) {
        copy_error(error, error_size, "native.install expects module, symbol and C source");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    unsigned long long id = 0;
    std::string install_error;
    if (!manager->install(value_string(arguments[0]), value_string(arguments[1]),
                          value_string(arguments[2]), &id, &install_error)) {
        copy_error(error, error_size, install_error);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    result->type = DEJAVU_VALUE_INTEGER;
    result->integer_value = static_cast<long long>(id);
    return DEJAVU_OK;
}

int NativeHookManager::lua_uninstall(dejavu_control_session *, void *context,
                                     const dejavu_value *arguments, size_t argument_count,
                                     dejavu_value *result, char *error, size_t error_size) {
    auto *manager = static_cast<NativeHookManager *>(context);
    if (manager == nullptr || argument_count != 1 || arguments[0].type != DEJAVU_VALUE_INTEGER ||
        arguments[0].integer_value <= 0) {
        copy_error(error, error_size, "native.uninstall expects a positive hook id");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::string uninstall_error;
    if (!manager->uninstall(static_cast<unsigned long long>(arguments[0].integer_value),
                            &uninstall_error)) {
        copy_error(error, error_size, uninstall_error);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    result->type = DEJAVU_VALUE_BOOLEAN;
    result->boolean_value = 1;
    return DEJAVU_OK;
}

int NativeHookManager::lua_list(dejavu_control_session *, void *context, const dejavu_value *,
                                size_t argument_count, dejavu_value *result, char *error,
                                size_t error_size) {
    auto *manager = static_cast<NativeHookManager *>(context);
    if (manager == nullptr || argument_count != 0) {
        copy_error(error, error_size, "native.list takes no arguments");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    if (!set_owned_string(result, manager->list())) {
        copy_error(error, error_size, "unable to allocate native hook list");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    return DEJAVU_OK;
}

int NativeHookManager::lua_stats(dejavu_control_session *, void *context,
                                 const dejavu_value *arguments, size_t argument_count,
                                 dejavu_value *result, char *error, size_t error_size) {
    auto *manager = static_cast<NativeHookManager *>(context);
    if (manager == nullptr || argument_count != 1 || arguments[0].type != DEJAVU_VALUE_INTEGER ||
        arguments[0].integer_value <= 0) {
        copy_error(error, error_size, "native.stats expects a positive hook id");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    unsigned long long hits = 0;
    std::string stats_error;
    if (!manager->stats(static_cast<unsigned long long>(arguments[0].integer_value), &hits,
                        &stats_error)) {
        copy_error(error, error_size, stats_error);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    result->type = DEJAVU_VALUE_INTEGER;
    result->integer_value = static_cast<long long>(hits);
    return DEJAVU_OK;
}

int NativeHookManager::lua_clear(dejavu_control_session *, void *context, const dejavu_value *,
                                 size_t argument_count, dejavu_value *result, char *error,
                                 size_t error_size) {
    auto *manager = static_cast<NativeHookManager *>(context);
    if (manager == nullptr || argument_count != 0) {
        copy_error(error, error_size, "native.clear takes no arguments");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    unsigned long long removed = 0;
    std::string clear_error;
    if (!manager->clear(&removed, &clear_error)) {
        copy_error(error, error_size, clear_error);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    result->type = DEJAVU_VALUE_INTEGER;
    result->integer_value = static_cast<long long>(removed);
    return DEJAVU_OK;
}
