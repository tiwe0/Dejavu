#include "plugin_manager.h"

#include <android/dlext.h>
#include <android/log.h>
#include <dobby.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "art_elf_resolver.h"
#include "sha256.h"
#include "dejavu_runtime.h"

namespace {

constexpr char kLogTag[] = "DejavuPlugin";
constexpr char kAutoLoadProperty[] = "debug.dejavu.plugins";
constexpr char kPluginManifest[] = "/data/adb/modules/dejavu_zygisk/config/plugins.txt";
constexpr char kAllowedProperty[] = "debug.dejavu.plugins.allowed";
constexpr char kAllowedHashProperty[] = "debug.dejavu.plugins.sha256";
constexpr char kAllowedSkillHashProperty[] = "debug.dejavu.plugins.skill.sha256";
constexpr int kAllowedPropertySlots = 16;
constexpr size_t kPluginIndexNone = static_cast<size_t>(-1);
constexpr size_t kMaximumSkillSize = 128 * 1024;

thread_local size_t g_loading_plugin_index = kPluginIndexNone;
thread_local size_t g_current_plugin_index = kPluginIndexNone;

size_t active_plugin_index() {
    return g_current_plugin_index != kPluginIndexNone ? g_current_plugin_index
                                                      : g_loading_plugin_index;
}

struct PluginContextGuard {
    size_t previous = g_current_plugin_index;
    explicit PluginContextGuard(size_t index) {
        g_current_plugin_index = index;
    }
    ~PluginContextGuard() {
        g_current_plugin_index = previous;
    }
};

#include "jit_prelude_generated.h"

struct JitValueLayout {
    int type;
    int owned;
    int boolean_value;
    long long integer_value;
    double number_value;
    const char *string_value;
    unsigned long string_size;
    void *handle_value;
    unsigned long long function_value;
    void *host_function;
    void *host_context;
};
static_assert(sizeof(JitValueLayout) == sizeof(dejavu_value), "JIT value layout mismatch");
static_assert(offsetof(JitValueLayout, string_value) == offsetof(dejavu_value, string_value),
              "JIT value offset mismatch");

using JitFunction = int (*)(void *, void *, const JitValueLayout *, unsigned long, JitValueLayout *,
                            char *, unsigned long);
using JitInitFunction = int (*)();
using JitShutdownFunction = void (*)();
using JitStringFunction = const char *(*)();
using JitUnloadSafeFunction = int (*)();

bool target_module_has_execute_only_text(void *target, std::string *error) {
    FILE *maps = fopen("/proc/self/maps", "re");
    if (maps == nullptr) {
        return false;
    }
    char line[512];
    std::string target_path;
    bool target_execute_only = false;
    while (fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long start = 0;
        unsigned long end = 0;
        char permissions[8] = {};
        if (sscanf(line, "%lx-%lx %7s", &start, &end, permissions) != 3) {
            continue;
        }
        const uintptr_t address = reinterpret_cast<uintptr_t>(target);
        if (address >= start && address < end) {
            if ((permissions[0] != 'r') && (permissions[2] == 'x')) {
                target_execute_only = true;
            }
            char *path = strchr(line, '/');
            if (path != nullptr) {
                target_path = path;
                while (!target_path.empty() &&
                       (target_path.back() == '\n' || target_path.back() == '\r')) {
                    target_path.pop_back();
                }
            }
            break;
        }
    }
    if (target_execute_only) {
        *error = "target mapping is execute-only; native hooks are unsupported on this ROM";
        fclose(maps);
        return true;
    }
    if (!target_path.empty()) {
        rewind(maps);
        while (fgets(line, sizeof(line), maps) != nullptr) {
            unsigned long start = 0;
            unsigned long end = 0;
            char permissions[8] = {};
            if (sscanf(line, "%lx-%lx %7s", &start, &end, permissions) != 3) {
                continue;
            }
            char *path = strchr(line, '/');
            if (path == nullptr) {
                continue;
            }
            std::string candidate(path);
            while (!candidate.empty() && (candidate.back() == '\n' || candidate.back() == '\r')) {
                candidate.pop_back();
            }
            if (candidate != target_path) {
                continue;
            }
            if ((permissions[0] != 'r') && (permissions[2] == 'x')) {
                *error =
                    "target module has execute-only text; native hooks are unsupported on this ROM";
                fclose(maps);
                return true;
            }
        }
    }
    fclose(maps);
    return false;
}

PluginManager *g_jit_manager = nullptr;
thread_local std::string g_jit_last_error;

int PluginManagerHostMemRead(void *host_context, uintptr_t address, void *buffer, size_t size);
int PluginManagerHostMemWrite(void *host_context, uintptr_t address, const void *buffer,
                              size_t size);
int PluginManagerHostMemQuery(void *host_context, uintptr_t address, uintptr_t *start,
                              uintptr_t *end, uint32_t *prot);
void *PluginManagerHostModuleResolve(void *host_context, const char *library, const char *symbol);
int PluginManagerHostHookInstall(void *host_context, void *target, void *replacement,
                                 void **original, char *error, size_t error_size);
int PluginManagerHostHookUninstall(void *host_context, void *target, char *error,
                                   size_t error_size);
int PluginManagerHostFsRead(void *host_context, const char *path, void *buffer, size_t capacity,
                            size_t *size_read);
int PluginManagerHostFsWrite(void *host_context, const char *path, const void *data, size_t size);
int PluginManagerHostFsExists(void *host_context, const char *path);
int PluginManagerHostFsSize(void *host_context, const char *path, unsigned long long *size);
int PluginManagerHostFsRemove(void *host_context, const char *path);
int PluginManagerHostStreamOpen(void *host_context, uint32_t transport, const char *endpoint,
                                char *error, size_t error_size);
int PluginManagerHostStreamWrite(void *host_context, int handle, const void *data, size_t size);
int PluginManagerHostStreamClose(void *host_context, int handle);

const DejavuMemInterface g_mem_interface = {
    DEJAVU_MEM_INTERFACE_VERSION, sizeof(DejavuMemInterface), &PluginManagerHostMemRead,
    &PluginManagerHostMemWrite,   &PluginManagerHostMemQuery,
};

const DejavuModuleInterface g_module_interface = {
    DEJAVU_MODULE_INTERFACE_VERSION,
    sizeof(DejavuModuleInterface),
    &PluginManagerHostModuleResolve,
};

const DejavuHookInterface g_hook_interface = {
    DEJAVU_HOOK_INTERFACE_VERSION,
    sizeof(DejavuHookInterface),
    &PluginManagerHostHookInstall,
    &PluginManagerHostHookUninstall,
};

const DejavuFsInterface g_fs_interface = {
    DEJAVU_FS_INTERFACE_VERSION, sizeof(DejavuFsInterface),  &PluginManagerHostFsRead,
    &PluginManagerHostFsWrite,   &PluginManagerHostFsExists, &PluginManagerHostFsSize,
    &PluginManagerHostFsRemove,
};

const DejavuStreamInterface g_stream_interface = {
    DEJAVU_STREAM_INTERFACE_VERSION,   sizeof(DejavuStreamInterface),
    &PluginManager::host_stream_open,  &PluginManager::host_stream_write,
    &PluginManager::host_stream_close,
};

bool valid_identifier_text(const char *value) {
    if (value == nullptr || *value == '\0') {
        return false;
    }
    for (const char *cursor = value; *cursor != '\0'; ++cursor) {
        const unsigned char ch = static_cast<unsigned char>(*cursor);
        if (cursor == value) {
            if (!(ch == '_' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))) {
                return false;
            }
        } else if (!(ch == '_' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                     (ch >= '0' && ch <= '9'))) {
            return false;
        }
    }
    return true;
}

void read_skill_text(const std::string &path, std::string *text) {
    const std::string sidecar = path + ".SKILL.md";
    std::string skill_path;
    struct stat information = {};
    if (lstat(sidecar.c_str(), &information) == 0 && S_ISREG(information.st_mode)) {
        skill_path = sidecar;
    } else {
        const size_t slash = path.find_last_of('/');
        const std::string directory = slash == std::string::npos ? "." : path.substr(0, slash);
        const std::string fallback = directory + "/SKILL.md";
        if (lstat(fallback.c_str(), &information) == 0 && S_ISREG(information.st_mode)) {
            skill_path = fallback;
        }
    }
    if (skill_path.empty()) {
        return;
    }
    std::ifstream skill_file(skill_path, std::ios::binary);
    if (!skill_file.is_open()) {
        return;
    }
    std::ostringstream buffer;
    char chunk[4096];
    size_t total = 0;
    while (skill_file && total < kMaximumSkillSize) {
        const size_t wanted = std::min(sizeof(chunk), kMaximumSkillSize - total);
        skill_file.read(chunk, static_cast<std::streamsize>(wanted));
        const size_t count = static_cast<size_t>(skill_file.gcount());
        if (count == 0) {
            break;
        }
        buffer.write(chunk, static_cast<std::streamsize>(count));
        total += count;
    }
    *text = buffer.str();
}

void trim_line(std::string *line) {
    const size_t first = line->find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        line->clear();
        return;
    }
    const size_t last = line->find_last_not_of(" \t\r\n");
    *line = line->substr(first, last - first + 1);
}

void log_line(int priority, const char *message) {
    __android_log_print(priority, kLogTag, "%s", message != nullptr ? message : "");
}

int PluginManagerHostFsRead(void *, const char *path, void *buffer, size_t capacity,
                            size_t *size_read) {
    if (path == nullptr || *path == '\0' || buffer == nullptr || capacity == 0 ||
        size_read == nullptr || std::strlen(path) > 4096) {
        return -1;
    }
    constexpr size_t kMaximumRead = 16 * 1024 * 1024;
    if (capacity > kMaximumRead) {
        capacity = kMaximumRead;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return -1;
    }
    file.read(static_cast<char *>(buffer), static_cast<std::streamsize>(capacity));
    if (file.bad()) {
        return -1;
    }
    *size_read = static_cast<size_t>(file.gcount());
    return 0;
}

int PluginManagerHostFsWrite(void *, const char *path, const void *data, size_t size) {
    if (path == nullptr || *path == '\0' || std::strlen(path) > 4096 ||
        (data == nullptr && size != 0) || size > 16 * 1024 * 1024) {
        return -1;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return -1;
    }
    if (size != 0) {
        file.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
    }
    return file.good() ? 0 : -1;
}

int PluginManagerHostFsExists(void *, const char *path) {
    struct stat information = {};
    if (path == nullptr || lstat(path, &information) != 0) {
        return 0;
    }
    return S_ISREG(information.st_mode) ? 1 : 0;
}

int PluginManagerHostFsSize(void *, const char *path, unsigned long long *size) {
    struct stat information = {};
    if (path == nullptr || size == nullptr || stat(path, &information) != 0 ||
        !S_ISREG(information.st_mode)) {
        return -1;
    }
    *size = static_cast<unsigned long long>(information.st_size);
    return 0;
}

int PluginManagerHostFsRemove(void *, const char *path) {
    if (path == nullptr || *path == '\0') {
        return -1;
    }
    return unlink(path) == 0 ? 0 : -1;
}

} // namespace

extern "C" int dejavu_jit_register(const char *namespace_name, const char *function_name,
                                   JitFunction function, void *context, char *error,
                                   unsigned long error_size) {
    if (g_jit_manager == nullptr) {
        std::snprintf(error, error_size, "JIT registration outside plugin initialization");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    return g_jit_manager->register_jit_function(namespace_name, function_name,
                                                reinterpret_cast<void *>(function), context, error,
                                                error_size)
               ? DEJAVU_OK
               : DEJAVU_ERROR_INVALID_ARGUMENT;
}

extern "C" void *dejavu_jit_alloc(unsigned long size) {
    return malloc(size);
}

extern "C" void dejavu_jit_free(void *pointer) {
    free(pointer);
}

extern "C" int dejavu_jit_log(int priority, const char *message) {
    log_line(priority, message);
    return 0;
}

extern "C" void *dejavu_jit_get_app_class_loader(void) {
    return g_jit_manager != nullptr ? g_jit_manager->app_class_loader() : nullptr;
}

extern "C" int dejavu_jit_mem_read(unsigned long long address, void *buffer, unsigned long size) {
    if (g_jit_manager == nullptr ||
        !g_jit_manager->active_plugin_has_capability(DEJAVU_PLUGIN_CAP_MEM)) {
        return -1;
    }
    return PluginManagerHostMemRead(g_jit_manager, static_cast<uintptr_t>(address), buffer,
                                    static_cast<size_t>(size));
}

extern "C" int dejavu_jit_mem_write(unsigned long long address, const void *buffer,
                                    unsigned long size) {
    if (g_jit_manager == nullptr ||
        !g_jit_manager->active_plugin_has_capability(DEJAVU_PLUGIN_CAP_MEM)) {
        return -1;
    }
    return PluginManagerHostMemWrite(g_jit_manager, static_cast<uintptr_t>(address), buffer,
                                     static_cast<size_t>(size));
}

extern "C" int dejavu_jit_mem_query(unsigned long long address, unsigned long long *start,
                                    unsigned long long *end, unsigned int *prot) {
    if (g_jit_manager == nullptr ||
        !g_jit_manager->active_plugin_has_capability(DEJAVU_PLUGIN_CAP_MEM)) {
        return -1;
    }
    uintptr_t host_start = 0;
    uintptr_t host_end = 0;
    uint32_t host_prot = 0;
    const int status = PluginManagerHostMemQuery(g_jit_manager, static_cast<uintptr_t>(address),
                                                 &host_start, &host_end, &host_prot);
    if (start != nullptr) {
        *start = static_cast<unsigned long long>(host_start);
    }
    if (end != nullptr) {
        *end = static_cast<unsigned long long>(host_end);
    }
    if (prot != nullptr) {
        *prot = host_prot;
    }
    return status;
}

extern "C" void *dejavu_jit_module_resolve(const char *library, const char *symbol) {
    if (g_jit_manager == nullptr ||
        !g_jit_manager->active_plugin_has_capability(DEJAVU_PLUGIN_CAP_MODULE)) {
        return nullptr;
    }
    return PluginManagerHostModuleResolve(g_jit_manager, library, symbol);
}

extern "C" int dejavu_jit_hook_install(void *target, void *replacement, void **original,
                                       char *error, unsigned long error_size) {
    if (g_jit_manager == nullptr ||
        !g_jit_manager->active_plugin_has_capability(DEJAVU_PLUGIN_CAP_HOOK)) {
        std::snprintf(error, error_size, "plugin lacks hook capability");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    return PluginManagerHostHookInstall(g_jit_manager, target, replacement, original, error,
                                        static_cast<size_t>(error_size));
}

extern "C" int dejavu_jit_hook_uninstall(void *target, char *error, unsigned long error_size) {
    if (g_jit_manager == nullptr ||
        !g_jit_manager->active_plugin_has_capability(DEJAVU_PLUGIN_CAP_HOOK)) {
        std::snprintf(error, error_size, "plugin lacks hook capability");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    return PluginManagerHostHookUninstall(g_jit_manager, target, error,
                                          static_cast<size_t>(error_size));
}

extern "C" int dejavu_jit_fs_read(const char *path, void *buffer, unsigned long capacity,
                                  unsigned long *size_read) {
    if (g_jit_manager == nullptr ||
        !g_jit_manager->active_plugin_has_capability(DEJAVU_PLUGIN_CAP_FS)) {
        return -1;
    }
    size_t actual_size = 0;
    const int status = PluginManagerHostFsRead(g_jit_manager, path, buffer,
                                               static_cast<size_t>(capacity), &actual_size);
    if (size_read != nullptr) {
        *size_read = static_cast<unsigned long>(actual_size);
    }
    return status;
}

extern "C" int dejavu_jit_fs_write(const char *path, const void *data, unsigned long size) {
    if (g_jit_manager == nullptr ||
        !g_jit_manager->active_plugin_has_capability(DEJAVU_PLUGIN_CAP_FS)) {
        return -1;
    }
    return PluginManagerHostFsWrite(g_jit_manager, path, data, static_cast<size_t>(size));
}

extern "C" int dejavu_jit_fs_exists(const char *path) {
    if (g_jit_manager == nullptr ||
        !g_jit_manager->active_plugin_has_capability(DEJAVU_PLUGIN_CAP_FS)) {
        return 0;
    }
    return PluginManagerHostFsExists(g_jit_manager, path);
}

extern "C" int dejavu_jit_fs_size(const char *path, unsigned long long *size) {
    if (g_jit_manager == nullptr ||
        !g_jit_manager->active_plugin_has_capability(DEJAVU_PLUGIN_CAP_FS)) {
        return -1;
    }
    return PluginManagerHostFsSize(g_jit_manager, path, size);
}

extern "C" int dejavu_jit_fs_remove(const char *path) {
    if (g_jit_manager == nullptr ||
        !g_jit_manager->active_plugin_has_capability(DEJAVU_PLUGIN_CAP_FS)) {
        return -1;
    }
    return PluginManagerHostFsRemove(g_jit_manager, path);
}

extern "C" int dejavu_jit_stream_open(unsigned int transport, const char *endpoint, char *error,
                                      unsigned long error_size) {
    if (g_jit_manager == nullptr ||
        !g_jit_manager->active_plugin_has_capability(DEJAVU_PLUGIN_CAP_STREAM)) {
        std::snprintf(error, error_size, "plugin lacks stream capability");
        return 0;
    }
    return PluginManagerHostStreamOpen(g_jit_manager, transport, endpoint, error, error_size);
}

extern "C" int dejavu_jit_stream_write(int handle, const void *data, unsigned long size) {
    if (g_jit_manager == nullptr ||
        !g_jit_manager->active_plugin_has_capability(DEJAVU_PLUGIN_CAP_STREAM)) {
        return -1;
    }
    return PluginManagerHostStreamWrite(g_jit_manager, handle, data, size);
}

extern "C" int dejavu_jit_stream_close(int handle) {
    if (g_jit_manager == nullptr ||
        !g_jit_manager->active_plugin_has_capability(DEJAVU_PLUGIN_CAP_STREAM)) {
        return -1;
    }
    return PluginManagerHostStreamClose(g_jit_manager, handle);
}

extern "C" void dejavu_jit_set_error(const char *message) {
    g_jit_last_error = message != nullptr ? message : "";
}

extern "C" int dejavu_jit_arg_type(const JitValueLayout *arguments, unsigned long count,
                                   unsigned long index) {
    if (arguments == nullptr || index >= count) {
        return -1;
    }
    return arguments[index].type;
}

extern "C" int dejavu_jit_arg_int(const JitValueLayout *arguments, unsigned long count,
                                  unsigned long index, long long *value) {
    if (arguments == nullptr || value == nullptr || index >= count ||
        arguments[index].type != DEJAVU_VALUE_INTEGER) {
        return 0;
    }
    *value = arguments[index].integer_value;
    return 1;
}

extern "C" int dejavu_jit_arg_boolean(const JitValueLayout *arguments, unsigned long count,
                                      unsigned long index, int *value) {
    if (arguments == nullptr || value == nullptr || index >= count ||
        arguments[index].type != DEJAVU_VALUE_BOOLEAN) {
        return 0;
    }
    *value = arguments[index].boolean_value;
    return 1;
}

extern "C" const char *dejavu_jit_arg_string(const JitValueLayout *arguments, unsigned long count,
                                             unsigned long index, unsigned long *size) {
    if (arguments == nullptr || index >= count || arguments[index].type != DEJAVU_VALUE_STRING ||
        arguments[index].string_value == nullptr) {
        return nullptr;
    }
    if (size != nullptr) {
        *size = static_cast<unsigned long>(arguments[index].string_size);
    }
    return arguments[index].string_value;
}

extern "C" void dejavu_jit_return_int(JitValueLayout *result, long long value) {
    if (result == nullptr) {
        return;
    }
    std::memset(result, 0, sizeof(*result));
    result->type = DEJAVU_VALUE_INTEGER;
    result->integer_value = value;
}

extern "C" void dejavu_jit_return_boolean(JitValueLayout *result, int value) {
    if (result == nullptr) {
        return;
    }
    std::memset(result, 0, sizeof(*result));
    result->type = DEJAVU_VALUE_BOOLEAN;
    result->boolean_value = value != 0 ? 1 : 0;
}

extern "C" void dejavu_jit_return_string(JitValueLayout *result, const char *text) {
    if (result == nullptr) {
        return;
    }
    std::memset(result, 0, sizeof(*result));
    if (text == nullptr) {
        result->type = DEJAVU_VALUE_NIL;
        return;
    }
    const size_t size = std::strlen(text);
    char *copy = static_cast<char *>(malloc(size + 1));
    if (copy == nullptr) {
        result->type = DEJAVU_VALUE_NIL;
        return;
    }
    std::memcpy(copy, text, size);
    copy[size] = '\0';
    result->type = DEJAVU_VALUE_STRING;
    result->owned = 1;
    result->string_value = copy;
    result->string_size = size;
}

PluginManager::PluginManager() : streams_(std::make_unique<StreamService>()) {
}

PluginManager::~PluginManager() {
    shutdown();
    if (app_class_loader_ != nullptr) {
        JNIEnv *env = nullptr;
        if (vm_ != nullptr &&
            vm_->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_OK &&
            env != nullptr) {
            env->DeleteGlobalRef(app_class_loader_);
            app_class_loader_ = nullptr;
        }
    }
}

void PluginManager::shutdown() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (g_jit_manager == this) {
        g_jit_manager = nullptr;
    }
    for (size_t index = 0; index < plugins_.size(); ++index) {
        Plugin *plugin = plugins_[index].get();
        if (plugin == nullptr || !plugin->loaded) {
            continue;
        }
        {
            PluginContextGuard context_guard(index);
            if (plugin->jit) {
                if (plugin->jit_shutdown != nullptr) {
                    plugin->jit_shutdown();
                }
            } else if (plugin->descriptor != nullptr && plugin->descriptor->shutdown != nullptr) {
                plugin->descriptor->shutdown(plugin->state);
            }
        }
        remove_hooks_for(index);
        if (plugin->stream_owner_token != 0) {
            streams_->close_owner(plugin->stream_owner_token);
        }
        if (plugin->jit) {
            if (plugin->jit_module != nullptr) {
                if (plugin->jit_cache_owned) {
                    release_jit_module(plugin->jit_cache_key, true);
                } else if (runtime_ != nullptr) {
                    runtime_->module_destroy(plugin->jit_module);
                }
                plugin->jit_module = nullptr;
            }
        } else if (plugin->handle != nullptr) {
            dlclose(plugin->handle);
            plugin->handle = nullptr;
        }
        invalidate_entries(index);
        plugin->loaded = false;
        plugin->state = nullptr;
    }
    remove_all_hooks();
    plugins_.clear();
    destroy_jit_cache();
}

bool PluginManager::initialize(DejavuRuntime *runtime, JavaVM *vm, std::string *error) {
    if (runtime == nullptr || vm == nullptr) {
        *error = "invalid plugin manager initialization argument";
        return false;
    }
    runtime_ = runtime;
    vm_ = vm;
    host_.abi_version = DEJAVU_PLUGIN_ABI_VERSION;
    host_.struct_size = sizeof(DejavuPluginHost);
    host_.runtime = runtime;
    host_.java_vm = vm;
    host_.host_context = this;
    host_.register_function = &PluginManager::host_register_function;
    host_.log_message = &PluginManager::host_log_message;
    host_.get_app_class_loader = &PluginManager::host_get_app_class_loader;
    host_.alloc = &PluginManager::host_alloc;
    host_.free = &PluginManager::host_free;
    host_.get_interface = &PluginManager::host_get_interface;
    refresh_allowed_paths();
    autoload_from_property();
    return true;
}

void PluginManager::autoload_from_property() {
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get(kAutoLoadProperty, value) <= 0) {
        return;
    }
    std::string list(value);
    std::string current;
    std::vector<std::string> paths;
    for (char character : list) {
        if (character == ':' || character == ';' || character == '\n' || character == ' ') {
            if (!current.empty()) {
                paths.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(character);
        }
    }
    if (!current.empty()) {
        paths.push_back(current);
    }
    for (const std::string &path : paths) {
        std::string status;
        std::string load_error;
        if (!load_plugin(path, &status, &load_error)) {
            log_line(ANDROID_LOG_ERROR,
                     ("plugin autoload failed for " + path + ": " + load_error).c_str());
        } else {
            log_line(ANDROID_LOG_INFO, ("plugin autoloaded: " + status).c_str());
        }
    }
}

bool PluginManager::load_plugin(const std::string &path, std::string *status, std::string *error) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    refresh_allowed_paths();
    return load_plugin_locked(path, status, error);
}

void PluginManager::refresh_allowed_paths() {
    allowed_paths_.clear();
    allowed_hashes_.clear();
    allowed_skill_hashes_.clear();
    auto add_property_list = [this](const char *property, const std::string *hash,
                                    const std::string *skill_hash) {
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get(property, value) <= 0) {
            return;
        }
        std::string list(value);
        std::vector<std::string> paths;
        std::string current;
        for (char character : list) {
            if (character == ':' || character == ';' || character == '\n' || character == ' ') {
                if (!current.empty()) {
                    paths.push_back(current);
                    current.clear();
                }
            } else {
                current.push_back(character);
            }
        }
        if (!current.empty()) {
            paths.push_back(current);
        }
        for (const std::string &path : paths) {
            allowed_paths_.insert(path);
        }
        if (paths.size() == 1) {
            if (hash != nullptr && !hash->empty()) {
                allowed_hashes_[paths.front()] = *hash;
            }
            if (skill_hash != nullptr && !skill_hash->empty()) {
                allowed_skill_hashes_[paths.front()] = *skill_hash;
            }
        }
    };
    add_property_list(kAllowedProperty, nullptr, nullptr);
    for (int slot = 0; slot < kAllowedPropertySlots; ++slot) {
        const std::string property = std::string(kAllowedProperty) + "." + std::to_string(slot);
        char hash_value[PROP_VALUE_MAX] = {};
        const std::string hash_property =
            std::string(kAllowedHashProperty) + "." + std::to_string(slot);
        std::string hash;
        if (__system_property_get(hash_property.c_str(), hash_value) > 0) {
            hash = hash_value;
        }
        char skill_hash_value[PROP_VALUE_MAX] = {};
        const std::string skill_hash_property =
            std::string(kAllowedSkillHashProperty) + "." + std::to_string(slot);
        std::string skill_hash;
        if (__system_property_get(skill_hash_property.c_str(), skill_hash_value) > 0) {
            skill_hash = skill_hash_value;
        }
        add_property_list(property.c_str(), &hash, &skill_hash);
    }
    load_plugin_manifest();
}

std::string PluginManager::expected_hash_for(const std::string &path) const {
    const auto iterator = allowed_hashes_.find(path);
    return iterator == allowed_hashes_.end() ? std::string() : iterator->second;
}

bool PluginManager::verify_skill_text(const std::string &path, const std::string &text,
                                      std::string *error) const {
    const auto iterator = allowed_skill_hashes_.find(path);
    if (iterator == allowed_skill_hashes_.end()) {
        return true;
    }
    if (text.empty()) {
        *error = "SKILL sidecar is pinned but missing: " + path;
        return false;
    }
    if (sha256_hex(text.data(), text.size()) != iterator->second) {
        *error = "SKILL sidecar hash mismatch: " + path;
        return false;
    }
    return true;
}

bool PluginManager::verify_file_hash(const std::string &path, std::string *error) const {
    const std::string expected = expected_hash_for(path);
    if (expected.empty()) {
        return true;
    }
    std::string actual;
    std::string hash_error;
    if (!sha256_file_hex(path, &actual, &hash_error)) {
        *error = hash_error;
        return false;
    }
    if (actual != expected) {
        *error = "plugin hash mismatch: " + path;
        return false;
    }
    return true;
}

void PluginManager::load_plugin_manifest() {
    std::ifstream file(kPluginManifest);
    if (!file.is_open()) {
        return;
    }
    std::string line;
    while (std::getline(file, line)) {
        trim_line(&line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        allowed_paths_.insert(line);
    }
}

bool PluginManager::whitelisted_path(const std::string &path) const {
    return allowed_paths_.find(path) != allowed_paths_.end();
}

bool PluginManager::name_exists(const std::string &name) const {
    for (const auto &plugin : plugins_) {
        if (plugin->name == name && plugin->loaded) {
            return true;
        }
    }
    return false;
}

void PluginManager::invalidate_entries(size_t plugin_index) {
    for (auto &entry : entries_) {
        if (entry->plugin_index == plugin_index) {
            entry->valid = false;
        }
    }
    pending_entries_.erase(std::remove_if(pending_entries_.begin(), pending_entries_.end(),
                                          [plugin_index](Entry *entry) {
                                              return entry->plugin_index == plugin_index;
                                          }),
                           pending_entries_.end());
}

void PluginManager::remove_plugin(size_t plugin_index) {
    if (plugin_index >= plugins_.size()) {
        return;
    }
    invalidate_entries(plugin_index);
    remove_hooks_for(plugin_index);
    plugins_.erase(plugins_.begin() + static_cast<std::ptrdiff_t>(plugin_index));
    for (auto &entry : entries_) {
        if (entry->plugin_index > plugin_index) {
            --entry->plugin_index;
        }
    }
    for (auto &hook : hooks_) {
        if (hook.plugin_index > plugin_index) {
            --hook.plugin_index;
        }
    }
}

bool PluginManager::remember_hook(void *target) {
    const size_t index = active_plugin_index();
    if (index == kPluginIndexNone || target == nullptr) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (const HookRecord &record : hooks_) {
        if (record.target == target) {
            return false;
        }
    }
    hooks_.push_back(HookRecord{target, index});
    return true;
}

void PluginManager::forget_hook(void *target) {
    const size_t index = active_plugin_index();
    if (index == kPluginIndexNone) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    hooks_.erase(std::remove_if(hooks_.begin(), hooks_.end(),
                                [target, index](const HookRecord &record) {
                                    return record.target == target && record.plugin_index == index;
                                }),
                 hooks_.end());
}

size_t PluginManager::hook_count_for(size_t plugin_index) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    size_t count = 0;
    for (const HookRecord &record : hooks_) {
        if (record.plugin_index == plugin_index) {
            ++count;
        }
    }
    return count;
}

bool PluginManager::active_plugin_has_capability(uint32_t capability) const {
    const size_t index = active_plugin_index();
    if (index == kPluginIndexNone) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (index >= plugins_.size()) {
        return false;
    }
    return (plugins_[index]->capabilities & capability) != 0;
}

bool PluginManager::uninstall_owned_hook(void *target, char *error, size_t error_size) {
    const size_t index = active_plugin_index();
    if (index == kPluginIndexNone || target == nullptr) {
        std::snprintf(error, error_size, "hook.uninstall is only allowed from plugin code");
        return false;
    }
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        bool owned = false;
        for (const HookRecord &record : hooks_) {
            if (record.target == target && record.plugin_index == index) {
                owned = true;
                break;
            }
        }
        if (!owned) {
            std::snprintf(error, error_size, "hook target is not owned by this plugin");
            return false;
        }
    }
    const int status = DobbyDestroy(target);
    if (status != 0) {
        std::snprintf(error, error_size, "DobbyDestroy failed with status %d", status);
        return false;
    }
    forget_hook(target);
    return true;
}

void PluginManager::remove_hooks_for(size_t plugin_index) {
    std::vector<void *> targets;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        for (auto iterator = hooks_.begin(); iterator != hooks_.end();) {
            if (iterator->plugin_index == plugin_index) {
                targets.push_back(iterator->target);
                iterator = hooks_.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }
    for (void *target : targets) {
        const int status = DobbyDestroy(target);
        if (status != 0) {
            log_line(
                ANDROID_LOG_WARN,
                ("DobbyDestroy failed during plugin teardown: " + std::to_string(status)).c_str());
        }
    }
}

void PluginManager::remove_all_hooks() {
    std::vector<void *> targets;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        for (const HookRecord &record : hooks_) {
            targets.push_back(record.target);
        }
        hooks_.clear();
    }
    for (void *target : targets) {
        DobbyDestroy(target);
    }
}

bool PluginManager::valid_identifier(const std::string &value) {
    return valid_identifier_text(value.c_str());
}

bool PluginManager::valid_namespace(const std::string &value) {
    static const std::set<std::string> reserved = {
        "hook", "fs", "module", "native", "java", "mem", "plugin",
    };
    return valid_identifier(value) && reserved.find(value) == reserved.end();
}

bool PluginManager::load_plugin_locked(const std::string &path, std::string *status,
                                       std::string *error) {
    if (!whitelisted_path(path)) {
        *error =
            "plugin is not in the trusted allowlist (debug.dejavu.plugins.allowed or plugins.txt)";
        return false;
    }
    for (const auto &plugin : plugins_) {
        if (plugin->path == path && plugin->loaded) {
            *status = plugin->name + " already loaded from " + path;
            return true;
        }
    }
    const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        *error = "unable to open plugin file securely";
        return false;
    }
    struct stat information = {};
    if (fstat(fd, &information) != 0 || !S_ISREG(information.st_mode)) {
        close(fd);
        *error = "plugin must be a regular file (no symlinks)";
        return false;
    }
    if ((information.st_mode & 0022) != 0) {
        close(fd);
        *error = "plugin file must not be group/world writable";
        return false;
    }
    if (information.st_uid != 0 && information.st_uid != getuid()) {
        close(fd);
        *error = "plugin file must be owned by root or the target uid";
        return false;
    }
    const std::string expected_hash = expected_hash_for(path);
    if (!expected_hash.empty()) {
        std::string actual_hash;
        std::string hash_error;
        if (!sha256_fd_hex(fd, static_cast<unsigned long long>(information.st_size), &actual_hash,
                           &hash_error)) {
            close(fd);
            *error = hash_error;
            return false;
        }
        if (actual_hash != expected_hash) {
            close(fd);
            *error = "plugin hash mismatch: " + path;
            return false;
        }
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        *error = "unable to rewind plugin file";
        return false;
    }
    android_dlextinfo dlext = {};
    dlext.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
    dlext.library_fd = fd;
    void *handle = android_dlopen_ext(path.c_str(), RTLD_NOW | RTLD_LOCAL, &dlext);
    close(fd);
    if (handle == nullptr) {
        const char *dl_error = dlerror();
        *error = dl_error != nullptr ? dl_error : "dlopen failed";
        return false;
    }
    auto entry = reinterpret_cast<DejaVuPluginEntry>(dlsym(handle, DEJAVU_PLUGIN_ENTRY_SYMBOL));
    if (entry == nullptr) {
        dlclose(handle);
        *error = "plugin does not export " DEJAVU_PLUGIN_ENTRY_SYMBOL;
        return false;
    }
    const DejavuPluginDescriptor *descriptor = entry();
    constexpr size_t descriptor_min_size =
        offsetof(DejavuPluginDescriptor, shutdown) + sizeof(void (*)(void *));
    if (descriptor == nullptr || descriptor->abi_version != DEJAVU_PLUGIN_ABI_VERSION ||
        descriptor->struct_size < descriptor_min_size || descriptor->name == nullptr ||
        descriptor->initialize == nullptr) {
        dlclose(handle);
        *error = "plugin descriptor is incompatible";
        return false;
    }
    const std::string plugin_name(descriptor->name);
    if (!valid_identifier(plugin_name)) {
        dlclose(handle);
        *error = "plugin name must be a simple identifier";
        return false;
    }
    for (size_t position = 0; position < plugins_.size();) {
        if (plugins_[position]->name == plugin_name && !plugins_[position]->loaded) {
            remove_plugin(position);
        } else {
            ++position;
        }
    }
    if (name_exists(plugin_name)) {
        dlclose(handle);
        *error = "plugin name already registered: " + plugin_name;
        return false;
    }
    std::string skill_text;
    read_skill_text(path, &skill_text);
    if (!verify_skill_text(path, skill_text, error)) {
        dlclose(handle);
        return false;
    }
    auto plugin = std::make_unique<Plugin>();
    plugin->handle = handle;
    plugin->skill_text = std::move(skill_text);
    plugin->descriptor = descriptor;
    plugin->path = path;
    plugin->name = plugin_name;
    plugin->version = descriptor->version != nullptr ? descriptor->version : "";
    plugin->flags = descriptor->flags;
    if (descriptor->struct_size >=
        offsetof(DejavuPluginDescriptor, capabilities) + sizeof(descriptor->capabilities)) {
        plugin->capabilities = descriptor->capabilities;
    }
    plugin->stream_owner_token = next_stream_owner_++;
    const size_t plugin_index = plugins_.size();
    plugins_.push_back(std::move(plugin));
    char plugin_error[512] = {};
    g_loading_plugin_index = plugin_index;
    const int init_status = descriptor->initialize(&host_, &plugins_[plugin_index]->state,
                                                   plugin_error, sizeof(plugin_error));
    g_loading_plugin_index = kPluginIndexNone;
    if (init_status != 0) {
        *error = plugin_error[0] != '\0' ? plugin_error : "plugin initialize failed";
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        streams_->close_owner(plugins_[plugin_index]->stream_owner_token);
        remove_plugin(plugin_index);
        dlclose(handle);
        return false;
    }
    plugins_[plugin_index]->loaded = true;
    if (session_ != nullptr && descriptor->on_control_ready != nullptr) {
        PluginContextGuard context_guard(plugin_index);
        descriptor->on_control_ready(plugins_[plugin_index]->state);
        plugins_[plugin_index]->control_ready = true;
    }
    *status =
        plugins_[plugin_index]->name +
        (plugins_[plugin_index]->version.empty() ? "" : " " + plugins_[plugin_index]->version);
    log_line(ANDROID_LOG_INFO, ("plugin loaded: " + *status + " from " + path).c_str());
    return true;
}

dejavu_module *PluginManager::compile_jit_source(const std::string &hash, const std::string &source,
                                                 std::string *error) {
    const auto cached = jit_cache_.find(hash);
    if (cached != jit_cache_.end() && cached->second.module != nullptr) {
        ++cached->second.refs;
        cached->second.stamp = ++jit_cache_clock_;
        log_line(ANDROID_LOG_DEBUG, "JIT cache hit");
        return cached->second.module;
    }
    const std::string full_source =
        std::string(kJitPrelude) + "\n#line 0 \"dejavu-jit-plugin.c\"\n" + source;
    dejavu_module *module = nullptr;
    char compile_error[1024] = {};
    const int compile_status = runtime_->engine_compile(engine_, full_source.c_str(), &module,
                                                        compile_error, sizeof(compile_error));
    if (compile_status != DEJAVU_OK || module == nullptr) {
        if (module != nullptr) {
            runtime_->module_destroy(module);
        }
        *error = compile_error[0] != '\0' ? compile_error : "JIT compilation failed";
        return nullptr;
    }
    constexpr size_t kMaximumCacheEntries = 4;
    if (jit_cache_.size() >= kMaximumCacheEntries) {
        auto victim = jit_cache_.end();
        for (auto iterator = jit_cache_.begin(); iterator != jit_cache_.end(); ++iterator) {
            if (iterator->second.refs == 0 &&
                (victim == jit_cache_.end() || iterator->second.stamp < victim->second.stamp)) {
                victim = iterator;
            }
        }
        if (victim != jit_cache_.end()) {
            runtime_->module_destroy(victim->second.module);
            jit_cache_.erase(victim);
        }
    }
    if (jit_cache_.size() < kMaximumCacheEntries) {
        JitCacheEntry entry;
        entry.module = module;
        entry.refs = 1;
        entry.stamp = ++jit_cache_clock_;
        jit_cache_[hash] = entry;
    }
    return module;
}

void PluginManager::release_jit_module(const std::string &hash, bool owned) {
    if (!owned) {
        return;
    }
    const auto iterator = jit_cache_.find(hash);
    if (iterator != jit_cache_.end() && iterator->second.refs > 0) {
        --iterator->second.refs;
    }
}

void PluginManager::discard_jit_module(const std::string &hash, dejavu_module *module,
                                       bool cache_owned) {
    if (module == nullptr) {
        return;
    }
    if (cache_owned) {
        release_jit_module(hash, true);
    } else if (runtime_ != nullptr) {
        runtime_->module_destroy(module);
    }
}

void PluginManager::destroy_jit_cache() {
    for (auto &entry : jit_cache_) {
        if (entry.second.module != nullptr && runtime_ != nullptr) {
            runtime_->module_destroy(entry.second.module);
        }
    }
    jit_cache_.clear();
}

bool PluginManager::load_jit_plugin(const std::string &path, std::string *status,
                                    std::string *error) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    refresh_allowed_paths();
    return load_jit_plugin_locked(path, status, error);
}

bool PluginManager::load_jit_plugin_locked(const std::string &path, std::string *status,
                                           std::string *error) {
    if (engine_ == nullptr || runtime_ == nullptr) {
        *error = "JIT plugins require the Lua control engine; load after start_control";
        return false;
    }
    if (!whitelisted_path(path)) {
        *error = "JIT source is not in the trusted allowlist";
        return false;
    }
    struct stat information = {};
    if (lstat(path.c_str(), &information) != 0 || !S_ISREG(information.st_mode)) {
        *error = "JIT source must be a regular file (no symlinks)";
        return false;
    }
    if ((information.st_mode & 0022) != 0) {
        *error = "JIT source must not be group/world writable";
        return false;
    }
    if (information.st_uid != 0 && information.st_uid != getuid()) {
        *error = "JIT source must be owned by root or the target uid";
        return false;
    }
    if (information.st_size <= 0 || information.st_size > 512 * 1024) {
        *error = "JIT source size is invalid (limit 512 KiB)";
        return false;
    }
    for (const auto &plugin : plugins_) {
        if (plugin->path == path && plugin->loaded) {
            *status = plugin->name + " already loaded from " + path;
            return true;
        }
    }
    std::ifstream source_file(path, std::ios::binary);
    if (!source_file.is_open()) {
        *error = "unable to read JIT source";
        return false;
    }
    std::ostringstream source_buffer;
    source_buffer << source_file.rdbuf();
    const std::string source = source_buffer.str();
    if (source.empty()) {
        *error = "JIT source is empty";
        return false;
    }
    const std::string source_hash = sha256_hex(source.data(), source.size());
    const std::string expected_hash = expected_hash_for(path);
    if (!expected_hash.empty() && source_hash != expected_hash) {
        *error = "plugin hash mismatch: " + path;
        return false;
    }
    dejavu_module *module = compile_jit_source(source_hash, source, error);
    if (module == nullptr) {
        return false;
    }
    const auto cache_entry = jit_cache_.find(source_hash);
    const bool cache_owned =
        cache_entry != jit_cache_.end() && cache_entry->second.module == module;
    auto symbol = [this, module](const char *name) {
        return runtime_->module_symbol(module, name);
    };
    auto *initialize = reinterpret_cast<JitInitFunction>(symbol("dejavu_jit_plugin_init"));
    if (initialize == nullptr) {
        discard_jit_module(source_hash, module, cache_owned);
        *error = "JIT source must define int dejavu_jit_plugin_init(void)";
        return false;
    }
    auto *shutdown = reinterpret_cast<JitShutdownFunction>(symbol("dejavu_jit_plugin_shutdown"));
    auto *name_function = reinterpret_cast<JitStringFunction>(symbol("dejavu_jit_plugin_name"));
    auto *version_function =
        reinterpret_cast<JitStringFunction>(symbol("dejavu_jit_plugin_version"));
    auto *unload_safe_function =
        reinterpret_cast<JitUnloadSafeFunction>(symbol("dejavu_jit_plugin_unload_safe"));
    auto *control_ready =
        reinterpret_cast<JitShutdownFunction>(symbol("dejavu_jit_plugin_control_ready"));
    auto *prepare =
        reinterpret_cast<JitShutdownFunction>(symbol("dejavu_jit_plugin_unload_prepare"));
    auto *capabilities_function =
        reinterpret_cast<JitUnloadSafeFunction>(symbol("dejavu_jit_plugin_capabilities"));
    std::string plugin_name;
    if (name_function != nullptr && name_function() != nullptr) {
        plugin_name = name_function();
    } else {
        size_t slash = path.find_last_of('/');
        std::string stem = slash == std::string::npos ? path : path.substr(slash + 1);
        const size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos) {
            stem.resize(dot);
        }
        plugin_name = stem;
    }
    if (!valid_identifier(plugin_name)) {
        discard_jit_module(source_hash, module, cache_owned);
        *error = "JIT plugin name must be a simple identifier";
        return false;
    }
    for (size_t position = 0; position < plugins_.size();) {
        if (plugins_[position]->name == plugin_name && !plugins_[position]->loaded) {
            remove_plugin(position);
        } else {
            ++position;
        }
    }
    if (name_exists(plugin_name)) {
        discard_jit_module(source_hash, module, cache_owned);
        *error = "plugin name already registered: " + plugin_name;
        return false;
    }
    auto plugin = std::make_unique<Plugin>();
    plugin->path = path;
    plugin->name = plugin_name;
    plugin->version =
        (version_function != nullptr && version_function() != nullptr) ? version_function() : "";
    plugin->flags = (unload_safe_function != nullptr && unload_safe_function() != 0)
                        ? DEJAVU_PLUGIN_FLAG_UNLOAD_SAFE
                        : 0;
    plugin->jit = true;
    plugin->stream_owner_token = next_stream_owner_++;
    plugin->jit_cache_owned = cache_owned;
    plugin->jit_cache_key = source_hash;
    plugin->capabilities =
        capabilities_function != nullptr ? static_cast<uint32_t>(capabilities_function()) : 0;
    std::string skill_text;
    read_skill_text(path, &skill_text);
    if (!verify_skill_text(path, skill_text, error)) {
        discard_jit_module(source_hash, module, cache_owned);
        return false;
    }
    plugin->skill_text = std::move(skill_text);
    plugin->jit_module = module;
    plugin->jit_shutdown = shutdown;
    plugin->jit_prepare = prepare;
    const size_t plugin_index = plugins_.size();
    plugins_.push_back(std::move(plugin));
    g_jit_last_error.clear();
    g_loading_plugin_index = plugin_index;
    g_jit_manager = this;
    const int init_status = initialize();
    g_loading_plugin_index = kPluginIndexNone;
    if (init_status != 0) {
        *error = !g_jit_last_error.empty() ? g_jit_last_error : "JIT plugin initialize failed";
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        streams_->close_owner(plugins_[plugin_index]->stream_owner_token);
        discard_jit_module(source_hash, module, cache_owned);
        remove_plugin(plugin_index);
        return false;
    }
    plugins_[plugin_index]->loaded = true;
    if (session_ != nullptr && control_ready != nullptr) {
        PluginContextGuard context_guard(plugin_index);
        control_ready();
        plugins_[plugin_index]->control_ready = true;
    }
    *status =
        plugins_[plugin_index]->name +
        (plugins_[plugin_index]->version.empty() ? "" : " " + plugins_[plugin_index]->version);
    log_line(ANDROID_LOG_INFO, ("JIT plugin loaded: " + *status + " from " + path).c_str());
    return true;
}

bool PluginManager::register_jit_function(const char *namespace_name, const char *function_name,
                                          void *function, void *function_context, char *error,
                                          size_t error_size) {
    if (function == nullptr) {
        std::snprintf(error, error_size, "JIT function pointer is null");
        return false;
    }
    const int status = host_register_function(this, namespace_name, function_name,
                                              reinterpret_cast<dejavu_host_function>(function),
                                              function_context, error, error_size);
    return status == DEJAVU_OK;
}

bool PluginManager::unload_plugin(const std::string &name, std::string *status,
                                  std::string *error) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Plugin *target = nullptr;
    size_t index = 0;
    for (size_t position = 0; position < plugins_.size(); ++position) {
        if (plugins_[position]->name == name && plugins_[position]->loaded) {
            target = plugins_[position].get();
            index = position;
            break;
        }
    }
    if (target == nullptr) {
        *error = "plugin not loaded: " + name;
        return false;
    }
    if ((target->flags & DEJAVU_PLUGIN_FLAG_UNLOAD_SAFE) == 0) {
        *error = "plugin is not marked unload-safe; remove its hooks explicitly or restart";
        return false;
    }
    for (const auto &entry : entries_) {
        if (entry->plugin_index == index && entry->in_flight.load() != 0) {
            *error = "plugin function is still executing: " + name;
            return false;
        }
    }
    {
        PluginContextGuard context_guard(index);
        if (target->jit) {
            if (target->jit_prepare != nullptr) {
                target->jit_prepare();
            }
        } else if (target->descriptor != nullptr &&
                   target->descriptor->struct_size >=
                       offsetof(DejavuPluginDescriptor, on_unload_prepare) +
                           sizeof(target->descriptor->on_unload_prepare) &&
                   target->descriptor->on_unload_prepare != nullptr) {
            target->descriptor->on_unload_prepare(target->state);
        }
    }
    const size_t owned_hooks = hook_count_for(index);
    if (owned_hooks != 0) {
        *error = "plugin still owns " + std::to_string(owned_hooks) +
                 " host hook(s); uninstall them in on_unload_prepare before unload";
        return false;
    }
    invalidate_entries(index);
    {
        PluginContextGuard context_guard(index);
        if (target->jit) {
            if (target->jit_shutdown != nullptr) {
                target->jit_shutdown();
            }
        } else if (target->descriptor != nullptr && target->descriptor->shutdown != nullptr) {
            target->descriptor->shutdown(target->state);
        }
    }
    remove_hooks_for(index);
    if (target->stream_owner_token != 0) {
        streams_->close_owner(target->stream_owner_token);
    }
    if (target->jit) {
        if (target->jit_module != nullptr) {
            if (target->jit_cache_owned) {
                release_jit_module(target->jit_cache_key, true);
            } else if (runtime_ != nullptr) {
                runtime_->module_destroy(target->jit_module);
            }
            target->jit_module = nullptr;
        }
    }
    if (!target->jit && target->handle != nullptr) {
        dlclose(target->handle);
        target->handle = nullptr;
    }
    remove_plugin(index);
    *status = name + " unloaded";
    return true;
}

bool PluginManager::register_lua(dejavu_control_session *session, std::string *error) {
    if (runtime_ == nullptr || session == nullptr) {
        *error = "plugin manager is not initialized";
        return false;
    }
    struct Registration {
        const char *name;
        dejavu_host_function function;
    };
    const Registration registrations[] = {
        {"load", &PluginManager::lua_load},     {"load_jit", &PluginManager::lua_load_jit},
        {"list", &PluginManager::lua_list},     {"status", &PluginManager::lua_status},
        {"unload", &PluginManager::lua_unload}, {"call", &PluginManager::lua_call},
        {"skill", &PluginManager::lua_skill},
    };
    char runtime_error[512];
    for (const Registration &registration : registrations) {
        const int status = runtime_->control_session_register(session, "plugin", registration.name,
                                                              registration.function, this,
                                                              runtime_error, sizeof(runtime_error));
        if (status != DEJAVU_OK) {
            *error = runtime_error;
            return false;
        }
    }
    registered_lua_ = true;
    return true;
}

bool PluginManager::attach_session(dejavu_control_session *session, dejavu_engine *engine,
                                   JNIEnv *env, jobject app_class_loader, std::string *error) {
    if (session == nullptr) {
        *error = "invalid plugin control session";
        return false;
    }
    session_ = session;
    engine_ = engine;
    g_jit_manager = this;
    if (engine_ != nullptr) {
        struct HostSymbol {
            const char *name;
            const void *address;
        };
        const HostSymbol host_symbols[] = {
            {"dejavu_jit_register", reinterpret_cast<const void *>(&dejavu_jit_register)},
            {"dejavu_jit_alloc", reinterpret_cast<const void *>(&dejavu_jit_alloc)},
            {"dejavu_jit_free", reinterpret_cast<const void *>(&dejavu_jit_free)},
            {"dejavu_jit_log", reinterpret_cast<const void *>(&dejavu_jit_log)},
            {"dejavu_jit_get_app_class_loader",
             reinterpret_cast<const void *>(&dejavu_jit_get_app_class_loader)},
            {"dejavu_jit_mem_read", reinterpret_cast<const void *>(&dejavu_jit_mem_read)},
            {"dejavu_jit_mem_write", reinterpret_cast<const void *>(&dejavu_jit_mem_write)},
            {"dejavu_jit_mem_query", reinterpret_cast<const void *>(&dejavu_jit_mem_query)},
            {"dejavu_jit_module_resolve",
             reinterpret_cast<const void *>(&dejavu_jit_module_resolve)},
            {"dejavu_jit_hook_install", reinterpret_cast<const void *>(&dejavu_jit_hook_install)},
            {"dejavu_jit_hook_uninstall",
             reinterpret_cast<const void *>(&dejavu_jit_hook_uninstall)},
            {"dejavu_jit_fs_read", reinterpret_cast<const void *>(&dejavu_jit_fs_read)},
            {"dejavu_jit_fs_write", reinterpret_cast<const void *>(&dejavu_jit_fs_write)},
            {"dejavu_jit_fs_exists", reinterpret_cast<const void *>(&dejavu_jit_fs_exists)},
            {"dejavu_jit_fs_size", reinterpret_cast<const void *>(&dejavu_jit_fs_size)},
            {"dejavu_jit_fs_remove", reinterpret_cast<const void *>(&dejavu_jit_fs_remove)},
            {"dejavu_jit_stream_open", reinterpret_cast<const void *>(&dejavu_jit_stream_open)},
            {"dejavu_jit_stream_write", reinterpret_cast<const void *>(&dejavu_jit_stream_write)},
            {"dejavu_jit_stream_close", reinterpret_cast<const void *>(&dejavu_jit_stream_close)},
            {"dejavu_jit_set_error", reinterpret_cast<const void *>(&dejavu_jit_set_error)},
            {"dejavu_jit_arg_type", reinterpret_cast<const void *>(&dejavu_jit_arg_type)},
            {"dejavu_jit_arg_int", reinterpret_cast<const void *>(&dejavu_jit_arg_int)},
            {"dejavu_jit_arg_boolean", reinterpret_cast<const void *>(&dejavu_jit_arg_boolean)},
            {"dejavu_jit_arg_string", reinterpret_cast<const void *>(&dejavu_jit_arg_string)},
            {"dejavu_jit_return_int", reinterpret_cast<const void *>(&dejavu_jit_return_int)},
            {"dejavu_jit_return_boolean",
             reinterpret_cast<const void *>(&dejavu_jit_return_boolean)},
            {"dejavu_jit_return_string", reinterpret_cast<const void *>(&dejavu_jit_return_string)},
        };
        for (const HostSymbol &symbol : host_symbols) {
            const int status = runtime_->engine_add_symbol(engine_, symbol.name, symbol.address);
            if (status != DEJAVU_OK) {
                *error = std::string("unable to register JIT host symbol ") + symbol.name;
                return false;
            }
        }
    }
    if (env != nullptr && app_class_loader != nullptr) {
        if (app_class_loader_ != nullptr) {
            env->DeleteGlobalRef(app_class_loader_);
        }
        app_class_loader_ = env->NewGlobalRef(app_class_loader);
    }
    if (!register_lua(session, error)) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (Entry *entry : pending_entries_) {
        entry->valid = true;
        char register_error[512];
        const int status = runtime_->control_session_register(
            session_, entry->namespace_name.c_str(), entry->function_name.c_str(),
            &PluginManager::trampoline, entry, register_error, sizeof(register_error));
        if (status != DEJAVU_OK) {
            entry->valid = false;
            *error = register_error;
            return false;
        }
    }
    pending_entries_.clear();
    for (auto &plugin : plugins_) {
        if (plugin->loaded && !plugin->control_ready && plugin->descriptor != nullptr &&
            plugin->descriptor->on_control_ready != nullptr) {
            plugin->descriptor->on_control_ready(plugin->state);
            plugin->control_ready = true;
        }
    }
    return true;
}

int PluginManager::host_register_function(void *host_context, const char *namespace_name,
                                          const char *function_name, dejavu_host_function function,
                                          void *function_context, char *error, size_t error_size) {
    auto *manager = static_cast<PluginManager *>(host_context);
    if (manager == nullptr || namespace_name == nullptr || function_name == nullptr ||
        function == nullptr) {
        std::snprintf(error, error_size, "plugin register_function got invalid arguments");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    const size_t plugin_index = g_loading_plugin_index;
    if (plugin_index == kPluginIndexNone) {
        std::snprintf(error, error_size,
                      "plugin functions can only be registered during initialize()");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    const std::string name_space(namespace_name);
    const std::string name_function(function_name);
    if (!PluginManager::valid_namespace(name_space)) {
        std::snprintf(error, error_size, "namespace '%s' is invalid or reserved by the framework",
                      namespace_name);
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    if (!PluginManager::valid_identifier(name_function)) {
        std::snprintf(error, error_size, "function name '%s' is invalid", function_name);
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::recursive_mutex> lock(manager->mutex_);
    for (const auto &existing : manager->entries_) {
        if (existing->valid && existing->namespace_name == name_space &&
            existing->function_name == name_function) {
            std::snprintf(error, error_size, "duplicate plugin function registration: %s.%s",
                          namespace_name, function_name);
            return DEJAVU_ERROR_INVALID_ARGUMENT;
        }
    }
    auto entry = std::make_unique<Entry>();
    entry->namespace_name = name_space;
    entry->function_name = name_function;
    entry->function = function;
    entry->context = function_context;
    entry->valid = true;
    entry->plugin_index = plugin_index;
    Entry *pointer = entry.get();
    manager->entries_.push_back(std::move(entry));
    if (manager->session_ != nullptr && manager->registered_lua_) {
        const int status = manager->runtime_->control_session_register(
            manager->session_, pointer->namespace_name.c_str(), pointer->function_name.c_str(),
            &PluginManager::trampoline, pointer, error, error_size);
        if (status != DEJAVU_OK) {
            pointer->valid = false;
            return status;
        }
    } else {
        manager->pending_entries_.push_back(pointer);
    }
    return DEJAVU_OK;
}

int PluginManager::host_log_message(void *, int priority, const char *message) {
    log_line(priority, message);
    return 0;
}

jobject PluginManager::host_get_app_class_loader(void *host_context) {
    auto *manager = static_cast<PluginManager *>(host_context);
    return manager != nullptr ? manager->app_class_loader_ : nullptr;
}

int PluginManager::trampoline(dejavu_control_session *session, void *context,
                              const dejavu_value *arguments, size_t argument_count,
                              dejavu_value *result, char *error, size_t error_size) {
    auto *entry = static_cast<Entry *>(context);
    if (entry == nullptr || !entry->valid || entry->function == nullptr) {
        std::snprintf(error, error_size, "plugin function is no longer loaded");
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    struct InFlightGuard {
        Entry *entry;
        explicit InFlightGuard(Entry *value) : entry(value) {
            entry->in_flight.fetch_add(1, std::memory_order_acq_rel);
        }
        ~InFlightGuard() {
            entry->in_flight.fetch_sub(1, std::memory_order_release);
        }
    } guard(entry);
    PluginContextGuard plugin_guard(entry->plugin_index);
    return entry->function(session, entry->context, arguments, argument_count, result, error,
                           error_size);
}

bool PluginManager::make_string(const std::string &value, dejavu_value *result) {
    char *copy = static_cast<char *>(malloc(value.size() + 1));
    if (copy == nullptr) {
        return false;
    }
    memcpy(copy, value.data(), value.size());
    copy[value.size()] = '\0';
    result->type = DEJAVU_VALUE_STRING;
    result->owned = 1;
    result->string_value = copy;
    result->string_size = value.size();
    return true;
}

bool PluginManager::lua_string_argument(const dejavu_value *arguments, size_t count, size_t index,
                                        std::string *value) {
    if (index >= count || arguments[index].type != DEJAVU_VALUE_STRING ||
        arguments[index].string_value == nullptr) {
        return false;
    }
    *value = std::string(arguments[index].string_value, arguments[index].string_size);
    return true;
}

int PluginManager::lua_load_jit(dejavu_control_session *, void *context,
                                const dejavu_value *arguments, size_t argument_count,
                                dejavu_value *result, char *error, size_t error_size) {
    auto *manager = static_cast<PluginManager *>(context);
    std::string path;
    if (manager == nullptr || argument_count != 1 ||
        !lua_string_argument(arguments, argument_count, 0, &path)) {
        std::snprintf(error, error_size, "plugin.load_jit expects a source file path");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::string status;
    std::string load_error;
    if (!manager->load_jit_plugin(path, &status, &load_error)) {
        std::snprintf(error, error_size, "%s", load_error.c_str());
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    if (!make_string(status, result)) {
        std::snprintf(error, error_size, "unable to allocate plugin status");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    return DEJAVU_OK;
}

int PluginManager::lua_load(dejavu_control_session *, void *context, const dejavu_value *arguments,
                            size_t argument_count, dejavu_value *result, char *error,
                            size_t error_size) {
    auto *manager = static_cast<PluginManager *>(context);
    std::string path;
    if (manager == nullptr || argument_count != 1 ||
        !lua_string_argument(arguments, argument_count, 0, &path)) {
        std::snprintf(error, error_size, "plugin.load expects an absolute .so path");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::string status;
    std::string load_error;
    if (!manager->load_plugin(path, &status, &load_error)) {
        std::snprintf(error, error_size, "%s", load_error.c_str());
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    if (!make_string(status, result)) {
        std::snprintf(error, error_size, "unable to allocate plugin status");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    return DEJAVU_OK;
}

std::string PluginManager::status() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    size_t valid_entries = 0;
    for (const auto &entry : entries_) {
        if (entry->valid) {
            ++valid_entries;
        }
    }
    std::ostringstream output;
    output << "plugins=" << plugins_.size() << " entries=" << valid_entries;
    for (const auto &plugin : plugins_) {
        output << " [" << plugin->name;
        if (!plugin->version.empty()) {
            output << " " << plugin->version;
        }
        output << " loaded=" << (plugin->loaded ? "yes" : "no")
               << " jit=" << (plugin->jit ? "yes" : "no") << " caps=0x" << std::hex
               << plugin->capabilities << std::dec
               << " skill=" << (plugin->skill_text.empty() ? "no" : "yes")
               << " path=" << plugin->path << "]";
    }
    return output.str();
}

int PluginManager::lua_list(dejavu_control_session *, void *context, const dejavu_value *,
                            size_t argument_count, dejavu_value *result, char *error,
                            size_t error_size) {
    auto *manager = static_cast<PluginManager *>(context);
    if (manager == nullptr || argument_count != 0) {
        std::snprintf(error, error_size, "plugin.list takes no arguments");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    if (!make_string(manager->status(), result)) {
        std::snprintf(error, error_size, "unable to allocate plugin status");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    return DEJAVU_OK;
}

int PluginManager::lua_status(dejavu_control_session *, void *context, const dejavu_value *,
                              size_t argument_count, dejavu_value *result, char *error,
                              size_t error_size) {
    return lua_list(nullptr, context, nullptr, argument_count, result, error, error_size);
}

int PluginManager::lua_unload(dejavu_control_session *, void *context,
                              const dejavu_value *arguments, size_t argument_count,
                              dejavu_value *result, char *error, size_t error_size) {
    auto *manager = static_cast<PluginManager *>(context);
    std::string name;
    if (manager == nullptr || argument_count != 1 ||
        !lua_string_argument(arguments, argument_count, 0, &name)) {
        std::snprintf(error, error_size, "plugin.unload expects a plugin name");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::string status;
    std::string unload_error;
    if (!manager->unload_plugin(name, &status, &unload_error)) {
        std::snprintf(error, error_size, "%s", unload_error.c_str());
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    if (!make_string(status, result)) {
        std::snprintf(error, error_size, "unable to allocate plugin status");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    return DEJAVU_OK;
}

int PluginManager::lua_skill(dejavu_control_session *, void *context, const dejavu_value *arguments,
                             size_t argument_count, dejavu_value *result, char *error,
                             size_t error_size) {
    auto *manager = static_cast<PluginManager *>(context);
    std::string name;
    if (manager == nullptr || argument_count != 1 ||
        !lua_string_argument(arguments, argument_count, 0, &name)) {
        std::snprintf(error, error_size, "plugin.skill expects a plugin name");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::recursive_mutex> lock(manager->mutex_);
    for (const auto &plugin : manager->plugins_) {
        if (plugin->name != name || !plugin->loaded) {
            continue;
        }
        if (plugin->skill_text.empty()) {
            std::snprintf(error, error_size, "plugin '%s' has no SKILL.md sidecar", name.c_str());
            return DEJAVU_ERROR_LUA_EXECUTE;
        }
        if (!make_string(plugin->skill_text, result)) {
            std::snprintf(error, error_size, "unable to allocate plugin skill");
            return DEJAVU_ERROR_OUT_OF_MEMORY;
        }
        return DEJAVU_OK;
    }
    std::snprintf(error, error_size, "plugin not loaded: %s", name.c_str());
    return DEJAVU_ERROR_LUA_EXECUTE;
}

int PluginManager::lua_call(dejavu_control_session *session, void *context,
                            const dejavu_value *arguments, size_t argument_count,
                            dejavu_value *result, char *error, size_t error_size) {
    auto *manager = static_cast<PluginManager *>(context);
    std::string plugin_name;
    std::string function_name;
    if (manager == nullptr || argument_count < 2 ||
        !lua_string_argument(arguments, argument_count, 0, &plugin_name) ||
        !lua_string_argument(arguments, argument_count, 1, &function_name)) {
        std::snprintf(error, error_size, "plugin.call expects plugin, function[, args...]");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    Entry *target_entry = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(manager->mutex_);
        size_t plugin_index = static_cast<size_t>(-1);
        for (size_t position = 0; position < manager->plugins_.size(); ++position) {
            if (manager->plugins_[position]->name == plugin_name &&
                manager->plugins_[position]->loaded) {
                plugin_index = position;
                break;
            }
        }
        if (plugin_index == static_cast<size_t>(-1)) {
            std::snprintf(error, error_size, "plugin not loaded: %s", plugin_name.c_str());
            return DEJAVU_ERROR_LUA_EXECUTE;
        }
        std::string qualified = function_name;
        const size_t dot = qualified.find('.');
        std::string requested_namespace =
            dot == std::string::npos ? std::string() : qualified.substr(0, dot);
        std::string requested_function =
            dot == std::string::npos ? qualified : qualified.substr(dot + 1);
        for (auto &entry : manager->entries_) {
            if (entry->plugin_index != plugin_index || !entry->valid) {
                continue;
            }
            if (entry->function_name != requested_function) {
                continue;
            }
            if (!requested_namespace.empty() && entry->namespace_name != requested_namespace) {
                continue;
            }
            target_entry = entry.get();
            break;
        }
    }
    if (target_entry != nullptr) {
        return trampoline(session, target_entry, arguments + 2, argument_count - 2, result, error,
                          error_size);
    }
    std::snprintf(error, error_size, "plugin function not found: %s", function_name.c_str());
    return DEJAVU_ERROR_LUA_EXECUTE;
}

void *PluginManager::host_alloc(size_t size) {
    return malloc(size);
}

void PluginManager::host_free(void *pointer) {
    free(pointer);
}

const void *PluginManager::host_get_interface(void *host_context, const char *name,
                                              uint32_t version) {
    auto *manager = static_cast<PluginManager *>(host_context);
    if (manager == nullptr || name == nullptr) {
        return nullptr;
    }
    const std::string interface_name(name);
    if (interface_name == "mem" && version == DEJAVU_MEM_INTERFACE_VERSION &&
        manager->active_plugin_has_capability(DEJAVU_PLUGIN_CAP_MEM)) {
        return &g_mem_interface;
    }
    if (interface_name == "module" && version == DEJAVU_MODULE_INTERFACE_VERSION &&
        manager->active_plugin_has_capability(DEJAVU_PLUGIN_CAP_MODULE)) {
        return &g_module_interface;
    }
    if (interface_name == "hook" && version == DEJAVU_HOOK_INTERFACE_VERSION &&
        manager->active_plugin_has_capability(DEJAVU_PLUGIN_CAP_HOOK)) {
        return &g_hook_interface;
    }
    if (interface_name == "fs" && version == DEJAVU_FS_INTERFACE_VERSION &&
        manager->active_plugin_has_capability(DEJAVU_PLUGIN_CAP_FS)) {
        return &g_fs_interface;
    }
    if (interface_name == "stream" && version == DEJAVU_STREAM_INTERFACE_VERSION &&
        manager->active_plugin_has_capability(DEJAVU_PLUGIN_CAP_STREAM)) {
        return &g_stream_interface;
    }
    return nullptr;
}

namespace {

int PluginManagerHostMemRead(void *, uintptr_t address, void *buffer, size_t size) {
    if (address == 0 || buffer == nullptr || size == 0) {
        return -1;
    }
    struct iovec local = {buffer, size};
    struct iovec remote = {reinterpret_cast<void *>(address), size};
    const ssize_t copied = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    return copied == static_cast<ssize_t>(size) ? 0 : -1;
}

int PluginManagerHostMemWrite(void *, uintptr_t address, const void *buffer, size_t size) {
    if (address == 0 || buffer == nullptr || size == 0) {
        return -1;
    }
    struct iovec local = {const_cast<void *>(buffer), size};
    struct iovec remote = {reinterpret_cast<void *>(address), size};
    const ssize_t copied = process_vm_writev(getpid(), &local, 1, &remote, 1, 0);
    return copied == static_cast<ssize_t>(size) ? 0 : -1;
}

int PluginManagerHostMemQuery(void *, uintptr_t address, uintptr_t *start, uintptr_t *end,
                              uint32_t *prot) {
    FILE *maps = fopen("/proc/self/maps", "r");
    if (maps == nullptr) {
        return -1;
    }
    char line[512];
    int result = -1;
    while (fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long mapping_start = 0;
        unsigned long mapping_end = 0;
        char permissions[8] = {};
        if (sscanf(line, "%lx-%lx %7s", &mapping_start, &mapping_end, permissions) != 3) {
            continue;
        }
        if (address < mapping_start || address >= mapping_end) {
            continue;
        }
        if (start != nullptr) {
            *start = static_cast<uintptr_t>(mapping_start);
        }
        if (end != nullptr) {
            *end = static_cast<uintptr_t>(mapping_end);
        }
        if (prot != nullptr) {
            uint32_t value = 0;
            if (permissions[0] == 'r')
                value |= 1u;
            if (permissions[1] == 'w')
                value |= 2u;
            if (permissions[2] == 'x')
                value |= 4u;
            *prot = value;
        }
        result = 0;
        break;
    }
    fclose(maps);
    return result;
}

void *PluginManagerHostModuleResolve(void *host_context, const char *library, const char *symbol) {
    auto *manager = static_cast<PluginManager *>(host_context);
    if (manager == nullptr || symbol == nullptr || *symbol == '\0') {
        return nullptr;
    }
    if (library == nullptr || *library == '\0') {
        return dlsym(RTLD_DEFAULT, symbol);
    }
    ArtElfResolver resolver;
    std::string error;
    if (resolver.open_loaded(library, &error)) {
        void *address = resolver.find(symbol);
        if (address != nullptr) {
            return address;
        }
    }
    void *handle = dlopen(library, RTLD_NOW | RTLD_NOLOAD);
    if (handle == nullptr) {
        return nullptr;
    }
    void *address = dlsym(handle, symbol);
    dlclose(handle);
    return address;
}

int PluginManagerHostStreamOpen(void *host_context, uint32_t transport, const char *endpoint,
                                char *error, size_t error_size) {
    return PluginManager::host_stream_open(host_context, transport, endpoint, error, error_size);
}

int PluginManagerHostStreamWrite(void *host_context, int handle, const void *data, size_t size) {
    return PluginManager::host_stream_write(host_context, handle, data, size);
}

int PluginManagerHostStreamClose(void *host_context, int handle) {
    return PluginManager::host_stream_close(host_context, handle);
}

int PluginManagerHostHookInstall(void *host_context, void *target, void *replacement,
                                 void **original, char *error, size_t error_size) {
    auto *manager = static_cast<PluginManager *>(host_context);
    if (manager == nullptr || target == nullptr || replacement == nullptr || original == nullptr) {
        std::snprintf(error, error_size, "hook.install requires target/replacement/original");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::string mapping_error;
    if (target_module_has_execute_only_text(target, &mapping_error)) {
        std::snprintf(error, error_size, "%s", mapping_error.c_str());
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    if (!manager->remember_hook(target)) {
        std::snprintf(error, error_size, "hook.install is only allowed from plugin code");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    const int status = DobbyHook(target, replacement, original);
    if (status != 0) {
        manager->forget_hook(target);
        std::snprintf(error, error_size, "DobbyHook failed with status %d", status);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    return DEJAVU_OK;
}

int PluginManagerHostHookUninstall(void *host_context, void *target, char *error,
                                   size_t error_size) {
    auto *manager = static_cast<PluginManager *>(host_context);
    if (manager == nullptr || target == nullptr) {
        std::snprintf(error, error_size, "hook.uninstall requires a target");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    if (!manager->uninstall_owned_hook(target, error, error_size)) {
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    return DEJAVU_OK;
}

} // namespace

int PluginManager::host_stream_open(void *host_context, uint32_t transport, const char *endpoint,
                                    char *error, size_t error_size) {
    auto *manager = static_cast<PluginManager *>(host_context);
    if (manager == nullptr || endpoint == nullptr) {
        std::snprintf(error, error_size, "stream.open requires an endpoint");
        return 0;
    }
    if (!manager->active_plugin_has_capability(DEJAVU_PLUGIN_CAP_STREAM)) {
        std::snprintf(error, error_size, "plugin lacks stream capability");
        return 0;
    }
    uint64_t token = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(manager->mutex_);
        const size_t index = active_plugin_index();
        if (index == kPluginIndexNone || index >= manager->plugins_.size() ||
            manager->plugins_[index]->stream_owner_token == 0) {
            std::snprintf(error, error_size, "stream.open is only allowed from loaded plugin code");
            return 0;
        }
        token = manager->plugins_[index]->stream_owner_token;
    }
    std::string stream_error;
    const int handle = manager->streams_->open(token, transport, endpoint, &stream_error);
    if (handle == 0) {
        std::snprintf(error, error_size, "%s",
                      stream_error.empty() ? "unable to open stream" : stream_error.c_str());
    }
    return handle;
}

int PluginManager::host_stream_write(void *host_context, int handle, const void *data,
                                     size_t size) {
    auto *manager = static_cast<PluginManager *>(host_context);
    if (manager == nullptr || handle <= 0 || (data == nullptr && size != 0)) {
        return -1;
    }
    return manager->streams_->write(handle, data, size);
}

int PluginManager::host_stream_close(void *host_context, int handle) {
    auto *manager = static_cast<PluginManager *>(host_context);
    if (manager == nullptr || handle <= 0) {
        return -1;
    }
    return manager->streams_->close(handle);
}
