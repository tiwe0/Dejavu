#include "native_hook_manager.h"

#include "art_elf_resolver.h"
#include "root_helper_client.h"

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
#include <fstream>
#include <limits>
#include <sstream>
#include <sys/stat.h>
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
extern void *dejavu_native_original_context(unsigned long long context);
extern void dejavu_native_hit_context(unsigned long long context);
#define dejavu_native_original() dejavu_native_original_context(DEJAVU_NATIVE_CONTEXT)
#define dejavu_native_hit() dejavu_native_hit_context(DEJAVU_NATIVE_CONTEXT)
)DEJAVU_NATIVE";

constexpr char kNativeCallPreamble[] = R"DEJAVU_CALL_API(
extern long long dejavu_native_call_argument(unsigned int index);
extern long long dejavu_native_call_argument_int(unsigned int index);
extern double dejavu_native_call_argument_double(unsigned int index);
extern int dejavu_native_call_argument_boolean(unsigned int index);
extern const char *dejavu_native_call_argument_string(unsigned int index, unsigned long *size);
extern int dejavu_native_call_argument_type(unsigned int index);
extern void dejavu_native_call_return(long long value);
)DEJAVU_CALL_API";

constexpr char kNativeCallEntry[] =
    "\nint dejavu_entry(void) { dejavu_native_call(); return 0; }\n";

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, kLogTag, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)

void copy_error(char *error, size_t error_size, const std::string &message) {
    if (error != nullptr && error_size != 0) {
        snprintf(error, error_size, "%s", message.c_str());
    }
}

bool read_source_file(const std::string &path, std::string *source, std::string *error) {
    struct stat information = {};
    if (path.empty() || lstat(path.c_str(), &information) != 0 || !S_ISREG(information.st_mode)) {
        *error = "C source path must be a regular file";
        return false;
    }
    if (information.st_size <= 0 || information.st_size > 1024 * 1024) {
        *error = "C source size is invalid (limit 1 MiB)";
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        *error = "unable to open C source file";
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    *source = buffer.str();
    if (source->empty()) {
        *error = "C source file is empty";
        return false;
    }
    return true;
}

std::string value_string(const dejavu_value &value) {
    return value.string_value == nullptr ? std::string()
                                         : std::string(value.string_value, value.string_size);
}

bool address_value(const dejavu_value &value, uintptr_t *address) {
    if (value.type == DEJAVU_VALUE_INTEGER) {
        if (value.integer_value <= 0 ||
            static_cast<unsigned long long>(value.integer_value) > std::numeric_limits<uintptr_t>::max())
            return false;
        *address = static_cast<uintptr_t>(value.integer_value);
        return true;
    }
    if (value.type != DEJAVU_VALUE_STRING) return false;
    const std::string text = value_string(value);
    if (text.size() < 3 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) return false;
    uintptr_t parsed = 0;
    for (size_t i = 2; i < text.size(); ++i) {
        const char c = text[i];
        unsigned digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        if (parsed > (std::numeric_limits<uintptr_t>::max() - digit) / 16) return false;
        parsed = parsed * 16 + digit;
    }
    *address = parsed;
    return parsed != 0;
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

std::string library_basename(const std::string &path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void *resolve_hidden_symbol(const std::string &module_name, const std::string &symbol_name) {
    ArtElfResolver resolver;
    std::string resolver_error;
    if (!resolver.open_loaded(library_basename(module_name), &resolver_error)) {
        return nullptr;
    }
    return resolver.find(symbol_name);
}

void *resolve_target(const std::string &module_name, const std::string &symbol_name,
                     void **handle_out, std::string *error) {
    void *handle = dlopen(module_name.c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (handle == nullptr) {
        const char *detail = dlerror();
        if (void *hidden = resolve_hidden_symbol(module_name, symbol_name)) {
            *handle_out = nullptr;
            return hidden;
        }
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
        if (void *hidden = resolve_hidden_symbol(module_name, symbol_name)) {
            dlclose(handle);
            *handle_out = nullptr;
            return hidden;
        }
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

// The saved range remains immutable even if Dobby splits the VMA.
class SavedProtectionScope {
  public:
    SavedProtectionScope(const RootHelperProtectionSnapshot &snapshot, std::atomic<bool> &pending)
        : snapshot_(snapshot), pending_(pending) {
    }
    ~SavedProtectionScope() {
        if (pending_.load(std::memory_order_acquire)) {
            std::string ignored;
            restore(&ignored);
        }
    }
    bool restore(std::string *error) {
        if (!pending_.load(std::memory_order_acquire))
            return true;
        const bool restored = root_helper_restore(snapshot_, error);
        pending_.store(!restored, std::memory_order_release);
        return restored;
    }

  private:
    const RootHelperProtectionSnapshot &snapshot_;
    std::atomic<bool> &pending_;
};

struct NativeCallContext {
    const std::vector<NativeCallArgument> *arguments = nullptr;
    bool result_set = false;
    long long result = 0;
    std::string error;
};

thread_local NativeCallContext *tls_call_context = nullptr;

const NativeCallArgument *current_argument(unsigned int index) {
    NativeCallContext *context = tls_call_context;
    if (context == nullptr || context->arguments == nullptr) {
        return nullptr;
    }
    if (index >= context->arguments->size()) {
        context->error = "native call argument index out of range: " + std::to_string(index);
        return nullptr;
    }
    return &(*context->arguments)[index];
}

void set_argument_type_error(unsigned int index, const char *expected) {
    NativeCallContext *context = tls_call_context;
    if (context != nullptr) {
        context->error = "native call argument " + std::to_string(index) + " expects " + expected;
    }
}

} // namespace

extern "C" void *dejavu_native_original_by_id(unsigned long long id);
extern "C" void dejavu_native_hit_by_id(unsigned long long id);
extern "C" void *dejavu_native_original_context(unsigned long long context);
extern "C" void dejavu_native_hit_context(unsigned long long context);
extern "C" void dejavu_native_log(const char *message);
extern "C" long long dejavu_native_call_argument(unsigned int index);
extern "C" long long dejavu_native_call_argument_int(unsigned int index);
extern "C" double dejavu_native_call_argument_double(unsigned int index);
extern "C" int dejavu_native_call_argument_boolean(unsigned int index);
extern "C" const char *dejavu_native_call_argument_string(unsigned int index, unsigned long *size);
extern "C" int dejavu_native_call_argument_type(unsigned int index);
extern "C" void dejavu_native_call_return(long long value);

std::atomic<NativeHookManager::Record *> NativeHookManager::callback_records_{nullptr};
std::atomic<unsigned long long> NativeHookManager::next_id_{1};

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
        {"dejavu_native_original_context",
         reinterpret_cast<const void *>(dejavu_native_original_context)},
        {"dejavu_native_hit_context", reinterpret_cast<const void *>(dejavu_native_hit_context)},
        {"dejavu_native_log", reinterpret_cast<const void *>(dejavu_native_log)},
        {"dejavu_native_call_argument",
         reinterpret_cast<const void *>(dejavu_native_call_argument)},
        {"dejavu_native_call_argument_int",
         reinterpret_cast<const void *>(dejavu_native_call_argument_int)},
        {"dejavu_native_call_argument_double",
         reinterpret_cast<const void *>(dejavu_native_call_argument_double)},
        {"dejavu_native_call_argument_boolean",
         reinterpret_cast<const void *>(dejavu_native_call_argument_boolean)},
        {"dejavu_native_call_argument_string",
         reinterpret_cast<const void *>(dejavu_native_call_argument_string)},
        {"dejavu_native_call_argument_type",
         reinterpret_cast<const void *>(dejavu_native_call_argument_type)},
        {"dejavu_native_call_return", reinterpret_cast<const void *>(dejavu_native_call_return)},
    };
    for (const HelperSymbol &helper : helpers) {
        if (runtime->engine_add_symbol(engine, helper.name, helper.address) != DEJAVU_OK) {
            *error = std::string("unable to register native hook helper: ") + helper.name;
            return false;
        }
    }
    runtime_ = runtime;
    engine_ = engine;
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
        {"call", NativeHookManager::lua_call},
        {"install_file", NativeHookManager::lua_install_file},
        {"install_address", NativeHookManager::lua_install_address},
        {"install_address_file", NativeHookManager::lua_install_address_file},
        {"call_file", NativeHookManager::lua_call_file},
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

void NativeHookManager::publish_original(void *original, size_t patch_size, void *context) {
    (void)patch_size; // The native backend rejects anything except an aligned 4-byte branch.
    auto *record = static_cast<Record *>(context);
    record->original.store(original, std::memory_order_release);
    if (!record->exposed.exchange(true, std::memory_order_acq_rel)) {
        Record *head = callback_records_.load(std::memory_order_acquire);
        do {
            record->callback_next = head;
        } while (!callback_records_.compare_exchange_weak(head, record, std::memory_order_release,
                                                          std::memory_order_acquire));
    }
}

void NativeHookManager::release_unexposed(Record *record) {
    if (record->exposed.load(std::memory_order_acquire) ||
        record->restore_pending.load(std::memory_order_acquire))
        return;
    if (record->module != nullptr) {
        runtime_->module_destroy(record->module);
        record->module = nullptr;
    }
    if (record->module_handle != nullptr) {
        dlclose(record->module_handle);
        record->module_handle = nullptr;
    }
}

bool NativeHookManager::install(const std::string &module_name, const std::string &symbol_name,
                                const std::string &source, unsigned long long *id,
                                std::string *error) {
    std::lock_guard<std::mutex> operation(operations_mutex_);
    if (runtime_ == nullptr || engine_ == nullptr) {
        *error = "native hook manager is not initialized";
        return false;
    }
    if (module_name.empty() || symbol_name.empty() || source.empty()) {
        *error = "native.install expects module, symbol and C source";
        return false;
    }
    auto owned = std::make_unique<Record>();
    Record *record = owned.get();
    record->id = next_id_.fetch_add(1, std::memory_order_relaxed);
    record->module_name = module_name;
    record->symbol_name = symbol_name;
    record->target = resolve_target(module_name, symbol_name, &record->module_handle, error);
    if (record->target == nullptr)
        return false;
    return install_resolved(std::move(owned), source, id, error);
}

bool NativeHookManager::install_address(uintptr_t address, const std::string &source,
                                       unsigned long long *id, std::string *error) {
    std::lock_guard<std::mutex> operation(operations_mutex_);
    if (runtime_ == nullptr || engine_ == nullptr) {
        *error = "native hook manager is not initialized";
        return false;
    }
    if (address == 0 || address % 4 != 0 || source.empty()) {
        *error = "native.install_address expects a nonzero four-byte-aligned address and C source";
        return false;
    }
    void *target = reinterpret_cast<void *>(address);
    int protection = 0;
    if (query_protection(target, &protection, error) != 0) return false;
    if ((protection & PROT_EXEC) == 0) {
        *error = "target address is not executable";
        return false;
    }
    auto owned = std::make_unique<Record>();
    owned->id = next_id_.fetch_add(1, std::memory_order_relaxed);
    owned->target = target;
    char text[32];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
    owned->symbol_name = text;
    owned->module_name = "<address>";
    // Pin a loaded image when the linker can identify it. Anonymous executable
    // mappings remain caller-owned and must outlive all entered replacements.
    Dl_info information{};
    if (dladdr(target, &information) != 0 && information.dli_fname != nullptr) {
        owned->module_name = information.dli_fname;
        owned->module_handle = dlopen(information.dli_fname, RTLD_NOW | RTLD_NOLOAD);
        if (owned->module_handle == nullptr) {
            *error = "unable to retain the loaded module for target address";
            return false;
        }
    }
    return install_resolved(std::move(owned), source, id, error);
}

bool NativeHookManager::install_resolved(std::unique_ptr<Record> owned, const std::string &source,
                                        unsigned long long *id, std::string *error) {
    Record *record = owned.get();
    if (DobbyIsHooked(record->target) != 0) {
        *error = "target already has a native hook";
        release_unexposed(record);
        return false;
    }
    if (!root_helper_capture(record->target, 4, &record->protection, error)) {
        release_unexposed(record);
        return false;
    }
    char context_address[48];
    std::snprintf(context_address, sizeof(context_address), "0x%llxULL",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(record)));
    std::string full_source = "#define DEJAVU_NATIVE_HOOK_ID " + std::to_string(record->id) +
                              "ULL\n#define DEJAVU_NATIVE_CONTEXT " + context_address + "\n" +
                              kNativePreamble + source;
    char compile_error[1024] = {};
    const int compile_status = runtime_->engine_compile(
        engine_, full_source.c_str(), &record->module, compile_error, sizeof(compile_error));
    if (compile_status != DEJAVU_OK) {
        *error = compile_error[0] != '\0' ? compile_error : "native hook compilation failed";
        release_unexposed(record);
        return false;
    }
    void *replacement = runtime_->module_symbol(record->module, kNativeTargetSymbol);
    if (replacement == nullptr) {
        *error = "C source must define dejavu_native_target with the target signature";
        release_unexposed(record);
        return false;
    }
    const unsigned long long hook_id = record->id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.emplace(hook_id, std::move(owned));
    }
    *id = hook_id;
    record->restore_pending.store(true, std::memory_order_release);
    SavedProtectionScope permissions(record->protection, record->restore_pending);
    if (!root_helper_apply_read_write(record->protection, error)) {
        std::string cleanup;
        if (!permissions.restore(&cleanup))
            *error += "; protection restore pending: " + cleanup;
        release_unexposed(record);
        *error = "native hook " + std::to_string(hook_id) + ": " + *error;
        return false;
    }
    const int hook_status =
        DobbyHookWithPrepare(record->target, replacement, publish_original, record);
    record->active.store(record->exposed.load(std::memory_order_acquire) &&
                             DobbyIsHooked(record->target) != 0,
                         std::memory_order_release);
    std::string restore_error;
    const bool restored = permissions.restore(&restore_error);
    if (hook_status != 0 || !restored) {
        // An exposed callback may still be running. Keep its original, JIT code,
        // target library reference and context until process exit, even on error.
        release_unexposed(record);
        *error = "native hook " + std::to_string(hook_id) + ": ";
        if (hook_status != 0)
            *error += "DobbyHook failed with status " + std::to_string(hook_status);
        if (!restored)
            *error += (hook_status != 0 ? "; " : "") + std::string("protection restore pending: ") +
                      restore_error;
        if (record->active.load(std::memory_order_acquire))
            *error +=
                "; hook remains installed; retry native.uninstall(" + std::to_string(hook_id) + ")";
        return false;
    }
    LOGD("native hook %llu installed: %s!%s target=%p original=%p", hook_id, record->module_name.c_str(),
         record->symbol_name.c_str(), record->target, record->original.load(std::memory_order_acquire));
    return true;
}

bool NativeHookManager::uninstall(unsigned long long id, std::string *error) {
    std::lock_guard<std::mutex> operation(operations_mutex_);
    return uninstall_locked(id, error);
}

bool NativeHookManager::uninstall_locked(unsigned long long id, std::string *error) {
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
    if (!record->active.load(std::memory_order_acquire) &&
        !record->restore_pending.load(std::memory_order_acquire)) {
        *error = "native hook is already uninstalled: " + std::to_string(id);
        return false;
    }
    record->restore_pending.store(true, std::memory_order_release);
    SavedProtectionScope permissions(record->protection, record->restore_pending);
    if (!root_helper_apply_read_write(record->protection, error)) {
        std::string cleanup;
        if (!permissions.restore(&cleanup))
            *error += "; protection restore pending: " + cleanup;
        return false;
    }
    int status = 0;
    if (record->active.load(std::memory_order_acquire)) {
        status = DobbyDestroy(record->target);
        record->active.store(DobbyIsHooked(record->target) != 0, std::memory_order_release);
    }
    std::string restore_error;
    const bool restored = permissions.restore(&restore_error);
    if (status != 0 || !restored) {
        *error = status != 0 ? "DobbyDestroy failed with status " + std::to_string(status) : "";
        if (!restored)
            *error += (status != 0 ? "; " : "") + std::string("protection restore pending: ") +
                      restore_error;
        return false;
    }
    release_unexposed(record);
    LOGD("native hook %llu uninstalled", id);
    return true;
}

bool NativeHookManager::clear(unsigned long long *removed, std::string *error) {
    std::lock_guard<std::mutex> operation(operations_mutex_);
    std::vector<unsigned long long> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &entry : records_) {
            if (entry.second->active.load(std::memory_order_acquire) ||
                entry.second->restore_pending.load(std::memory_order_acquire)) {
                ids.push_back(entry.first);
            }
        }
    }
    unsigned long long count = 0;
    for (const unsigned long long id : ids) {
        std::string uninstall_error;
        if (!uninstall_locked(id, &uninstall_error)) {
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
        if (record->restore_pending.load(std::memory_order_acquire))
            output += "protection-pending ";
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

bool NativeHookManager::call(const std::string &module_name, const std::string &symbol_name,
                             const std::string &source,
                             const std::vector<NativeCallArgument> &arguments, long long *result,
                             std::string *error) {
    if (runtime_ == nullptr || engine_ == nullptr) {
        *error = "native hook manager is not initialized";
        return false;
    }
    if (module_name.empty() || symbol_name.empty() || source.empty()) {
        *error = "native.call expects module, symbol and C source";
        return false;
    }
    if (arguments.size() > 32) {
        *error = "native.call accepts at most 32 integer arguments";
        return false;
    }
    void *module_handle = nullptr;
    void *target = resolve_target(module_name, symbol_name, &module_handle, error);
    if (target == nullptr) {
        return false;
    }
    int protection = 0;
    if (query_protection(target, &protection, error) != 0) {
        dlclose(module_handle);
        return false;
    }
    if ((protection & PROT_EXEC) == 0) {
        dlclose(module_handle);
        *error = "target symbol is not in an executable mapping";
        return false;
    }
    char address[32];
    snprintf(address, sizeof(address), "0x%llxULL",
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(target)));
    std::string full_source = "#define DEJAVU_NATIVE_CALL_ADDRESS ";
    full_source += address;
    full_source += "\n";
    full_source += kNativeCallPreamble;
    full_source += source;
    full_source += kNativeCallEntry;
    char compile_error[1024] = {};
    dejavu_module *module = nullptr;
    const int compile_status = runtime_->engine_compile(engine_, full_source.c_str(), &module,
                                                        compile_error, sizeof(compile_error));
    if (compile_status != DEJAVU_OK) {
        if (module != nullptr) {
            runtime_->module_destroy(module);
        }
        dlclose(module_handle);
        *error = compile_error[0] != '\0' ? compile_error : "native call compilation failed";
        return false;
    }
    NativeCallContext context;
    context.arguments = &arguments;
    tls_call_context = &context;
    int call_result = 0;
    const int call_status = runtime_->module_call(module, &call_result);
    tls_call_context = nullptr;
    runtime_->module_destroy(module);
    dlclose(module_handle);
    if (!context.error.empty()) {
        *error = context.error;
        return false;
    }
    if (call_status != DEJAVU_OK) {
        *error = "native call execution failed with status " + std::to_string(call_status);
        return false;
    }
    if (!context.result_set) {
        *error = "dejavu_native_call did not report a return value";
        return false;
    }
    *result = context.result;
    return true;
}

void NativeHookManager::shutdown() {
    std::lock_guard<std::mutex> operation(operations_mutex_);
    for (const auto &entry : records_) {
        Record *record = entry.second.get();
        if (record->active.load(std::memory_order_acquire) ||
            record->restore_pending.load(std::memory_order_acquire)) {
            std::string error;
            if (!uninstall_locked(entry.first, &error))
                LOGE("native hook %llu cleanup deferred: %s", entry.first, error.c_str());
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto &entry : records_) {
            Record *record = entry.second.get();
            if (record->exposed.load(std::memory_order_acquire) ||
                record->restore_pending.load(std::memory_order_acquire)) {
                // There is no safe quiescence proof for arbitrary native C callbacks.
                // The OS reclaims this pinned state when the process exits.
                entry.second.release();
            } else
                release_unexposed(record);
        }
        records_.clear();
    }
    runtime_ = nullptr;
    engine_ = nullptr;
}

void *NativeHookManager::original_for_context(unsigned long long context) {
    auto *record = reinterpret_cast<Record *>(static_cast<uintptr_t>(context));
    return record == nullptr ? nullptr : record->original.load(std::memory_order_acquire);
}

void NativeHookManager::hit_for_context(unsigned long long context) {
    auto *record = reinterpret_cast<Record *>(static_cast<uintptr_t>(context));
    if (record != nullptr)
        record->hits.fetch_add(1, std::memory_order_relaxed);
}

void *NativeHookManager::original_for_id(unsigned long long id) {
    for (Record *record = callback_records_.load(std::memory_order_acquire); record != nullptr;
         record = record->callback_next) {
        if (record->id == id)
            return record->original.load(std::memory_order_acquire);
    }
    return nullptr;
}

void NativeHookManager::hit_for_id(unsigned long long id) {
    for (Record *record = callback_records_.load(std::memory_order_acquire); record != nullptr;
         record = record->callback_next) {
        if (record->id == id) {
            record->hits.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

void NativeHookManager::log_message(const char *message) {
    __android_log_write(ANDROID_LOG_INFO, kLogTag, message != nullptr ? message : "(null)");
}

long long NativeHookManager::call_argument(unsigned int index) {
    const NativeCallArgument *argument = current_argument(index);
    if (argument == nullptr) {
        return 0;
    }
    switch (argument->type) {
    case NativeCallArgument::kNull:
        return 0;
    case NativeCallArgument::kBoolean:
        return argument->boolean_value ? 1 : 0;
    case NativeCallArgument::kInteger:
        return argument->integer_value;
    case NativeCallArgument::kNumber:
        return static_cast<long long>(argument->number_value);
    case NativeCallArgument::kString:
        set_argument_type_error(index, "an integer, got a string");
        return 0;
    }
    return 0;
}

long long NativeHookManager::call_argument_int(unsigned int index) {
    const NativeCallArgument *argument = current_argument(index);
    if (argument == nullptr) {
        return 0;
    }
    if (argument->type != NativeCallArgument::kInteger) {
        set_argument_type_error(index, "an integer");
        return 0;
    }
    return argument->integer_value;
}

double NativeHookManager::call_argument_double(unsigned int index) {
    const NativeCallArgument *argument = current_argument(index);
    if (argument == nullptr) {
        return 0.0;
    }
    if (argument->type == NativeCallArgument::kNumber) {
        return argument->number_value;
    }
    if (argument->type == NativeCallArgument::kInteger) {
        return static_cast<double>(argument->integer_value);
    }
    set_argument_type_error(index, "a number");
    return 0.0;
}

int NativeHookManager::call_argument_boolean(unsigned int index) {
    const NativeCallArgument *argument = current_argument(index);
    if (argument == nullptr) {
        return 0;
    }
    if (argument->type == NativeCallArgument::kBoolean) {
        return argument->boolean_value ? 1 : 0;
    }
    if (argument->type == NativeCallArgument::kInteger &&
        (argument->integer_value == 0 || argument->integer_value == 1)) {
        return static_cast<int>(argument->integer_value);
    }
    set_argument_type_error(index, "a boolean");
    return 0;
}

const char *NativeHookManager::call_argument_string(unsigned int index, unsigned long *size) {
    if (size != nullptr) {
        *size = 0;
    }
    const NativeCallArgument *argument = current_argument(index);
    if (argument == nullptr) {
        return nullptr;
    }
    if (argument->type == NativeCallArgument::kNull) {
        return nullptr;
    }
    if (argument->type != NativeCallArgument::kString) {
        set_argument_type_error(index, "a string");
        return nullptr;
    }
    if (size != nullptr) {
        *size = static_cast<unsigned long>(argument->string_value.size());
    }
    return argument->string_value.c_str();
}

int NativeHookManager::call_argument_type(unsigned int index) {
    const NativeCallArgument *argument = current_argument(index);
    return argument == nullptr ? -1 : static_cast<int>(argument->type);
}

void NativeHookManager::call_return(long long value) {
    NativeCallContext *context = tls_call_context;
    if (context == nullptr) {
        return;
    }
    context->result = value;
    context->result_set = true;
}

extern "C" void *dejavu_native_original_by_id(unsigned long long id) {
    return NativeHookManager::original_for_id(id);
}

extern "C" void dejavu_native_hit_by_id(unsigned long long id) {
    NativeHookManager::hit_for_id(id);
}

extern "C" void *dejavu_native_original_context(unsigned long long context) {
    return NativeHookManager::original_for_context(context);
}

extern "C" void dejavu_native_hit_context(unsigned long long context) {
    NativeHookManager::hit_for_context(context);
}

extern "C" void dejavu_native_log(const char *message) {
    NativeHookManager::log_message(message);
}

extern "C" long long dejavu_native_call_argument(unsigned int index) {
    return NativeHookManager::call_argument(index);
}

extern "C" long long dejavu_native_call_argument_int(unsigned int index) {
    return NativeHookManager::call_argument_int(index);
}

extern "C" double dejavu_native_call_argument_double(unsigned int index) {
    return NativeHookManager::call_argument_double(index);
}

extern "C" int dejavu_native_call_argument_boolean(unsigned int index) {
    return NativeHookManager::call_argument_boolean(index);
}

extern "C" const char *dejavu_native_call_argument_string(unsigned int index, unsigned long *size) {
    return NativeHookManager::call_argument_string(index, size);
}

extern "C" int dejavu_native_call_argument_type(unsigned int index) {
    return NativeHookManager::call_argument_type(index);
}

extern "C" void dejavu_native_call_return(long long value) {
    NativeHookManager::call_return(value);
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

int NativeHookManager::lua_install_address_impl(bool from_file, void *context,
                                                const dejavu_value *arguments, size_t argument_count,
                                                dejavu_value *result, char *error, size_t error_size) {
    auto *manager = static_cast<NativeHookManager *>(context);
    uintptr_t address = 0;
    if (manager == nullptr || argument_count != 2 ||
        !address_value(arguments[0], &address) || arguments[1].type != DEJAVU_VALUE_STRING) {
        copy_error(error, error_size, from_file
            ? "native.install_address_file expects a positive integer/0xHEX address and device C source path"
            : "native.install_address expects a positive integer/0xHEX address and C source");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::string source = value_string(arguments[1]);
    std::string detail;
    if (from_file && !read_source_file(value_string(arguments[1]), &source, &detail)) {
        copy_error(error, error_size, detail);
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    unsigned long long id = 0;
    if (!manager->install_address(address, source, &id, &detail)) {
        copy_error(error, error_size, detail);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    result->type = DEJAVU_VALUE_INTEGER;
    result->integer_value = static_cast<long long>(id);
    return DEJAVU_OK;
}

int NativeHookManager::lua_install_address(dejavu_control_session *, void *context,
                                           const dejavu_value *arguments, size_t argument_count,
                                           dejavu_value *result, char *error, size_t error_size) {
    return lua_install_address_impl(false, context, arguments, argument_count, result, error, error_size);
}

int NativeHookManager::lua_install_address_file(dejavu_control_session *, void *context,
                                                const dejavu_value *arguments, size_t argument_count,
                                                dejavu_value *result, char *error, size_t error_size) {
    return lua_install_address_impl(true, context, arguments, argument_count, result, error, error_size);
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

int NativeHookManager::lua_call(dejavu_control_session *, void *context,
                                const dejavu_value *arguments, size_t argument_count,
                                dejavu_value *result, char *error, size_t error_size) {
    auto *manager = static_cast<NativeHookManager *>(context);
    if (manager == nullptr || argument_count < 3 || argument_count > 3 + 32 ||
        arguments[0].type != DEJAVU_VALUE_STRING || arguments[1].type != DEJAVU_VALUE_STRING ||
        arguments[2].type != DEJAVU_VALUE_STRING) {
        copy_error(error, error_size,
                   "native.call expects module, symbol, C source and optional typed arguments");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::vector<NativeCallArgument> call_arguments;
    for (size_t index = 3; index < argument_count; ++index) {
        NativeCallArgument argument;
        switch (arguments[index].type) {
        case DEJAVU_VALUE_NIL:
            argument.type = NativeCallArgument::kNull;
            break;
        case DEJAVU_VALUE_BOOLEAN:
            argument.type = NativeCallArgument::kBoolean;
            argument.boolean_value = arguments[index].boolean_value != 0;
            break;
        case DEJAVU_VALUE_INTEGER:
            argument.type = NativeCallArgument::kInteger;
            argument.integer_value = arguments[index].integer_value;
            break;
        case DEJAVU_VALUE_NUMBER:
            argument.type = NativeCallArgument::kNumber;
            argument.number_value = arguments[index].number_value;
            break;
        case DEJAVU_VALUE_STRING:
            argument.type = NativeCallArgument::kString;
            argument.string_value = value_string(arguments[index]);
            break;
        default:
            copy_error(error, error_size,
                       "native.call arguments support nil, boolean, integer, number and string");
            return DEJAVU_ERROR_INVALID_ARGUMENT;
        }
        call_arguments.push_back(std::move(argument));
    }
    long long call_result = 0;
    std::string call_error;
    if (!manager->call(value_string(arguments[0]), value_string(arguments[1]),
                       value_string(arguments[2]), call_arguments, &call_result, &call_error)) {
        copy_error(error, error_size, call_error);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    result->type = DEJAVU_VALUE_INTEGER;
    result->integer_value = call_result;
    return DEJAVU_OK;
}

int NativeHookManager::lua_install_file(dejavu_control_session *, void *context,
                                        const dejavu_value *arguments, size_t argument_count,
                                        dejavu_value *result, char *error, size_t error_size) {
    auto *manager = static_cast<NativeHookManager *>(context);
    if (manager == nullptr || argument_count != 3 || arguments[0].type != DEJAVU_VALUE_STRING ||
        arguments[1].type != DEJAVU_VALUE_STRING || arguments[2].type != DEJAVU_VALUE_STRING) {
        copy_error(error, error_size,
                   "native.install_file expects module, symbol and device C source path");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::string source;
    std::string read_error;
    if (!read_source_file(value_string(arguments[2]), &source, &read_error)) {
        copy_error(error, error_size, read_error);
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    unsigned long long id = 0;
    std::string install_error;
    if (!manager->install(value_string(arguments[0]), value_string(arguments[1]), source, &id,
                          &install_error)) {
        copy_error(error, error_size, install_error);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    result->type = DEJAVU_VALUE_INTEGER;
    result->integer_value = static_cast<long long>(id);
    return DEJAVU_OK;
}

int NativeHookManager::lua_call_file(dejavu_control_session *, void *context,
                                     const dejavu_value *arguments, size_t argument_count,
                                     dejavu_value *result, char *error, size_t error_size) {
    auto *manager = static_cast<NativeHookManager *>(context);
    if (manager == nullptr || argument_count < 3 || argument_count > 3 + 32 ||
        arguments[0].type != DEJAVU_VALUE_STRING || arguments[1].type != DEJAVU_VALUE_STRING ||
        arguments[2].type != DEJAVU_VALUE_STRING) {
        copy_error(error, error_size,
                   "native.call_file expects module, symbol, device C source path and args");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::string source;
    std::string read_error;
    if (!read_source_file(value_string(arguments[2]), &source, &read_error)) {
        copy_error(error, error_size, read_error);
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::vector<NativeCallArgument> call_arguments;
    for (size_t index = 3; index < argument_count; ++index) {
        NativeCallArgument argument;
        switch (arguments[index].type) {
        case DEJAVU_VALUE_NIL:
            argument.type = NativeCallArgument::kNull;
            break;
        case DEJAVU_VALUE_BOOLEAN:
            argument.type = NativeCallArgument::kBoolean;
            argument.boolean_value = arguments[index].boolean_value != 0;
            break;
        case DEJAVU_VALUE_INTEGER:
            argument.type = NativeCallArgument::kInteger;
            argument.integer_value = arguments[index].integer_value;
            break;
        case DEJAVU_VALUE_NUMBER:
            argument.type = NativeCallArgument::kNumber;
            argument.number_value = arguments[index].number_value;
            break;
        case DEJAVU_VALUE_STRING:
            argument.type = NativeCallArgument::kString;
            argument.string_value = value_string(arguments[index]);
            break;
        default:
            copy_error(
                error, error_size,
                "native.call_file arguments support nil, boolean, integer, number and string");
            return DEJAVU_ERROR_INVALID_ARGUMENT;
        }
        call_arguments.push_back(std::move(argument));
    }
    long long call_result = 0;
    std::string call_error;
    if (!manager->call(value_string(arguments[0]), value_string(arguments[1]), source,
                       call_arguments, &call_result, &call_error)) {
        copy_error(error, error_size, call_error);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    result->type = DEJAVU_VALUE_INTEGER;
    result->integer_value = call_result;
    return DEJAVU_OK;
}
