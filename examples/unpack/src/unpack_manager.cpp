#include "unpack_manager.h"

#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "art_elf_resolver.h"

extern "C" void dejavu_unpack_invoke_with_arg_array(void *soa, void *method, void *argument_array,
                                                    void *result, const char *error_message);
extern "C" jvalue dejavu_unpack_invoke_with_jvalues(void *soa, jobject receiver, void *method,
                                                    const jvalue *arguments);
extern "C" void dejavu_unpack_art_method_invoke(void *method, void *thread, uint32_t *args,
                                                uint32_t args_size, void *result,
                                                const char *shorty);
extern "C" jvalue dejavu_unpack_invoke_with_varargs(void *soa, jobject receiver, void *method,
                                                    const void *arguments);

namespace {

constexpr char kLogTag[] = "DejavuUnpack";
constexpr size_t kMaximumDumpSize = 64 * 1024 * 1024;
constexpr char kDexMagic[] = "dex\n";
constexpr char kCdexMagic[] = "cdex001";
constexpr uintptr_t kAddressTagMask = 0x00FFFFFFFFFFFFFFULL;
uintptr_t normalize_address(uintptr_t address) {
    return address & kAddressTagMask;
}

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, kLogTag, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)

UnpackManager *g_unpack = nullptr;
void *g_dlopen_original = nullptr;
void *g_android_dlopen_ext_original = nullptr;
void *g_register_dexfile_original = nullptr;
void *g_register_dexfile_locked_original = nullptr;
void *g_opencommon_original = nullptr;
void *g_invoke_with_arg_array_original = nullptr;
void *g_invoke_with_jvalues_original = nullptr;
void *g_art_method_invoke_target = nullptr;
void *g_art_method_invoke_original = nullptr;
void *g_invoke_with_varargs_original = nullptr;
thread_local bool g_inside_dlopen_hook = false;
thread_local bool g_active_invoke = false;

struct MemoryMapping {
    uintptr_t start = 0;
    uintptr_t end = 0;
    std::string permissions;
    std::string path;
};

bool read_mappings(std::vector<MemoryMapping> *mappings, std::string *error) {
    std::ifstream maps("/proc/self/maps");
    if (!maps.is_open()) {
        *error = std::string("unable to open /proc/self/maps: ") + strerror(errno);
        return false;
    }
    mappings->clear();
    std::string line;
    while (std::getline(maps, line)) {
        unsigned long start = 0;
        unsigned long end = 0;
        char permissions[8] = {};
        int consumed = 0;
        if (sscanf(line.c_str(), "%lx-%lx %7s %n", &start, &end, permissions, &consumed) != 3) {
            continue;
        }
        MemoryMapping mapping;
        mapping.start = static_cast<uintptr_t>(start);
        mapping.end = static_cast<uintptr_t>(end);
        mapping.permissions = permissions;
        if (consumed > 0 && static_cast<size_t>(consumed) < line.size()) {
            size_t position = static_cast<size_t>(consumed);
            while (position < line.size() && line[position] == ' ') {
                ++position;
            }
            if (position < line.size()) {
                // Skip "offset dev inode" columns before the path.
                int fields = 0;
                while (position < line.size() && fields < 3) {
                    while (position < line.size() && line[position] != ' ') {
                        ++position;
                    }
                    while (position < line.size() && line[position] == ' ') {
                        ++position;
                    }
                    ++fields;
                }
                mapping.path = line.substr(position);
            }
        }
        mappings->push_back(std::move(mapping));
    }
    return true;
}

const std::vector<MemoryMapping> &cached_mappings(std::string *error) {
    static thread_local std::vector<MemoryMapping> cache;
    static thread_local std::chrono::steady_clock::time_point stamp;
    static thread_local bool initialized = false;
    const auto now = std::chrono::steady_clock::now();
    if (!initialized ||
        std::chrono::duration_cast<std::chrono::milliseconds>(now - stamp).count() > 100) {
        std::vector<MemoryMapping> fresh;
        std::string read_error;
        if (read_mappings(&fresh, &read_error)) {
            cache = std::move(fresh);
        } else if (cache.empty()) {
            *error = read_error;
        }
        stamp = now;
        initialized = true;
    }
    return cache;
}

bool find_mapping(uintptr_t address, MemoryMapping *mapping, std::string *error) {
    address = normalize_address(address);
    const std::vector<MemoryMapping> &mappings = cached_mappings(error);
    if (mappings.empty()) {
        if (error->empty()) {
            *error = "unable to read process mappings";
        }
        return false;
    }
    for (const MemoryMapping &candidate : mappings) {
        if (address >= candidate.start && address < candidate.end) {
            *mapping = candidate;
            return true;
        }
    }
    *error = "address is not mapped";
    return false;
}

bool is_file_backed(uintptr_t address) {
    MemoryMapping mapping;
    std::string ignored;
    if (!find_mapping(address, &mapping, &ignored)) {
        return false;
    }
    if (mapping.path.empty() || mapping.path[0] == '[') {
        return false;
    }
    return mapping.path.find("(deleted)") == std::string::npos;
}

bool safe_read(uintptr_t address, void *destination, size_t size) {
    address = normalize_address(address);
    const uintptr_t last = address + size - 1;
    if (size == 0 || last < address) {
        return false;
    }
    std::vector<MemoryMapping> mappings;
    std::string ignored;
    if (!read_mappings(&mappings, &ignored)) {
        return false;
    }
    for (const MemoryMapping &mapping : mappings) {
        if (address >= mapping.start && last < mapping.end && mapping.permissions.size() >= 3 &&
            mapping.permissions[0] == 'r') {
            memcpy(destination, reinterpret_cast<const void *>(address), size);
            return true;
        }
    }
    return false;
}

bool page_permissions(void *address, int *protection) {
    MemoryMapping mapping;
    std::string ignored;
    if (!find_mapping(reinterpret_cast<uintptr_t>(address), &mapping, &ignored)) {
        return false;
    }
    int result = 0;
    if (mapping.permissions.size() >= 3) {
        if (mapping.permissions[0] == 'r') {
            result |= PROT_READ;
        }
        if (mapping.permissions[1] == 'w') {
            result |= PROT_WRITE;
        }
        if (mapping.permissions[2] == 'x') {
            result |= PROT_EXEC;
        }
    }
    *protection = result;
    return true;
}

bool set_page_permissions(void *address, int protection, std::string *error) {
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        *error = "unable to determine the page size";
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

bool host_install_hook(const DejavuHookInterface *interface, void *host_context, void *target,
                       void *replacement, void **original, std::string *error) {
    if (interface == nullptr || interface->install == nullptr) {
        *error = "plugin host hook interface is unavailable";
        return false;
    }
    char hook_error[256] = {};
    const int status = interface->install(host_context, target, replacement, original, hook_error,
                                          sizeof(hook_error));
    if (status != DEJAVU_OK) {
        *error = hook_error[0] != '\0' ? hook_error : "host hook install failed";
        return false;
    }
    return true;
}

uint32_t read_u32(const unsigned char *bytes, size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

bool looks_like_dex(const unsigned char *header, size_t available, size_t *image_size) {
    if (available < 0x40) {
        return false;
    }
    const bool is_dex = memcmp(header, kDexMagic, 4) == 0;
    const bool is_cdex = memcmp(header, kCdexMagic, 7) == 0;
    if (!is_dex && !is_cdex) {
        return false;
    }
    const uint32_t file_size = read_u32(header, 0x20);
    const uint32_t header_size = read_u32(header, 0x24);
    const uint32_t endian = read_u32(header, 0x28);
    if (endian != 0x12345678 || header_size < 0x70 || header_size > 0x200) {
        return false;
    }
    if (file_size < header_size || file_size > kMaximumDumpSize) {
        return false;
    }
    *image_size = file_size;
    return true;
}

} // namespace

bool UnpackManager::install_hook(void *target, void *replacement, void **original,
                                 std::string *error) {
    return host_install_hook(hook_interface_, host_ != nullptr ? host_->host_context : nullptr,
                             target, replacement, original, error);
}

extern "C" void *dejavu_unpack_dlopen(const char *filename, int flags);
extern "C" void *dejavu_unpack_android_dlopen_ext(const char *filename, int flags,
                                                  const void *info);
extern "C" void dejavu_unpack_register_dex_file(void *self, void *dex_file, void *loader);
extern "C" void dejavu_unpack_register_dex_file_locked(void *self, void *dex_file, void *dex_cache,
                                                       void *loader);
extern "C" unsigned long dejavu_unpack_open_common(unsigned long a0, unsigned long a1,
                                                   unsigned long a2, unsigned long a3,
                                                   unsigned long a4, unsigned long a5,
                                                   unsigned long a6, unsigned long a7);

UnpackManager *UnpackManager::instance_ = nullptr;

UnpackManager::UnpackManager() = default;

UnpackManager::~UnpackManager() {
    shutdown();
}

UnpackManager *UnpackManager::instance() {
    return instance_;
}

bool UnpackManager::initialize(const DejavuPluginHost *host, std::string *error) {
    if (host == nullptr || host->java_vm == nullptr) {
        *error = "invalid unpack plugin initialization argument";
        return false;
    }
    host_ = host;
    vm_ = host->java_vm;
    if (host->get_interface != nullptr) {
        hook_interface_ = static_cast<const DejavuHookInterface *>(
            host->get_interface(host->host_context, "hook", DEJAVU_HOOK_INTERFACE_VERSION));
    }
    if (hook_interface_ == nullptr || hook_interface_->install == nullptr) {
        *error = "unpack plugin requires the host hook interface";
        return false;
    }
    instance_ = this;
    g_unpack = this;
    return true;
}

void UnpackManager::shutdown() {
    enabled_.store(false);
    stop_writer();
    if (g_unpack == this) {
        g_unpack = nullptr;
    }
    if (instance_ == this) {
        instance_ = nullptr;
    }
    host_ = nullptr;
}

bool UnpackManager::register_plugin_functions(const DejavuPluginHost *host, std::string *error) {
    if (host == nullptr || host->register_function == nullptr || host->host_context == nullptr) {
        *error = "unpack plugin host cannot register Lua functions";
        return false;
    }
    struct Registration {
        const char *name;
        dejavu_host_function function;
    };
    const Registration registrations[] = {
        {"enable", UnpackManager::lua_enable},
        {"disable", UnpackManager::lua_disable},
        {"status", UnpackManager::lua_status},
        {"install_art_hooks", UnpackManager::lua_install_art_hooks},
        {"dump_memory", UnpackManager::lua_dump_memory},
        {"include_file_backed", UnpackManager::lua_include_file_backed},
        {"hook_invoke", UnpackManager::lua_hook_invoke},
        {"active_invoke", UnpackManager::lua_active_invoke},
        {"flush", UnpackManager::lua_flush},
        {"active_invoke_async", UnpackManager::lua_active_invoke_async},
        {"active_invoke_wait", UnpackManager::lua_active_invoke_wait},
        {"active_invoke_status", UnpackManager::lua_active_invoke_status},
        {"sleep_ms", UnpackManager::lua_sleep_ms},
        {"dump_revision", UnpackManager::lua_dump_revision},
    };
    char register_error[512];
    for (const Registration &registration : registrations) {
        const int status = host->register_function(host->host_context, "unpack", registration.name,
                                                   registration.function, this, register_error,
                                                   sizeof(register_error));
        if (status != DEJAVU_OK) {
            *error = register_error;
            return false;
        }
    }
    return true;
}

bool UnpackManager::enable(const std::string &directory, std::string *error) {
    if (directory.empty()) {
        *error = "unpack.enable requires an output directory";
        return false;
    }
    if (mkdir(directory.c_str(), 0755) != 0 && errno != EEXIST) {
        *error = std::string("unable to create unpack directory: ") + strerror(errno);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        directory_ = directory;
    }
    enabled_.store(true);
    start_writer();
    std::string hook_error;
    if (!install_dlopen_hooks(&hook_error)) {
        LOGE("dlopen hook installation failed: %s", hook_error.c_str());
    }
    std::string art_error;
    if (!install_art_hooks(&art_error)) {
        LOGE("ART hook installation failed: %s", art_error.c_str());
    }
    return true;
}

void UnpackManager::enable_from_system_property() {
    char value[PROP_VALUE_MAX] = {};
    const int length = __system_property_get("debug.dejavu.unpack.dir", value);
    if (length <= 0 || value[0] == '\0') {
        return;
    }
    char include_value[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.dejavu.unpack.include_file_backed", include_value) > 0 &&
        (strcmp(include_value, "1") == 0 || strcmp(include_value, "true") == 0)) {
        include_file_backed_.store(true);
    }
    std::string error;
    if (!enable(value, &error)) {
        LOGE("auto unpack enable failed: %s", error.c_str());
        return;
    }
    LOGD("auto unpack enabled: dir=%s status=%s", value, status().c_str());
}

bool UnpackManager::install_dlopen_hooks(std::string *error) {
    if (dlopen_hooked_) {
        return true;
    }
    void *library = dlopen("libdl.so", RTLD_NOW | RTLD_NOLOAD);
    void *dlopen_address = library == nullptr ? nullptr : dlsym(library, "dlopen");
    void *android_ext_address = library == nullptr ? nullptr : dlsym(library, "android_dlopen_ext");
    if (dlopen_address == nullptr) {
        dlopen_address = dlsym(RTLD_DEFAULT, "dlopen");
    }
    if (android_ext_address == nullptr) {
        android_ext_address = dlsym(RTLD_DEFAULT, "android_dlopen_ext");
    }
    if (library != nullptr) {
        dlclose(library);
    }
    if (dlopen_address == nullptr) {
        *error = "dlopen symbol is unavailable";
        return false;
    }
    if (!install_hook(dlopen_address, reinterpret_cast<void *>(dejavu_unpack_dlopen),
                      &g_dlopen_original, error)) {
        return false;
    }
    if (android_ext_address != nullptr && android_ext_address != dlopen_address) {
        std::string ext_error;
        if (!install_hook(android_ext_address,
                          reinterpret_cast<void *>(dejavu_unpack_android_dlopen_ext),
                          &g_android_dlopen_ext_original, &ext_error)) {
            LOGE("android_dlopen_ext hook failed: %s", ext_error.c_str());
        }
    }
    dlopen_hooked_ = true;
    LOGD("dlopen hook installed: dlopen=%p android_dlopen_ext=%p", dlopen_address,
         android_ext_address);
    return true;
}

bool UnpackManager::install_art_hooks(std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (art_hooked_) {
        return true;
    }
    ArtElfResolver resolver;
    std::string resolver_error;
    if (!resolver.open_loaded("libart.so", &resolver_error)) {
        *error = "unable to inspect libart.so: " + resolver_error;
        return false;
    }
    std::vector<ArtElfSymbol> symbols;
    if (!resolver.enumerate_symbols(&symbols, &resolver_error)) {
        *error = "unable to enumerate libart symbols: " + resolver_error;
        return false;
    }
    unsigned long long register_address = 0;
    unsigned long long register_locked_address = 0;
    unsigned long long opencommon_address = 0;
    std::string register_name;
    std::string register_locked_name;
    std::string opencommon_name;
    for (const ArtElfSymbol &symbol : symbols) {
        if (symbol.name.find("RegisterDexFile") != std::string::npos) {
            if (symbol.name.find("Locked") != std::string::npos) {
                if (register_locked_address == 0) {
                    register_locked_address = symbol.address;
                    register_locked_name = symbol.name;
                }
            } else if (register_address == 0) {
                register_address = symbol.address;
                register_name = symbol.name;
            }
        }
        if (opencommon_address == 0 && symbol.name.find("OpenCommon") != std::string::npos) {
            opencommon_address = symbol.address;
            opencommon_name = symbol.name;
        }
    }
    std::string hook_error;
    if (register_address != 0) {
        if (install_hook(reinterpret_cast<void *>(register_address),
                         reinterpret_cast<void *>(dejavu_unpack_register_dex_file),
                         &g_register_dexfile_original, &hook_error)) {
            register_dexfile_address_ = register_address;
            LOGD("ART RegisterDexFile hook installed: %s @0x%llx", register_name.c_str(),
                 register_address);
        } else {
            LOGE("RegisterDexFile hook failed: %s", hook_error.c_str());
        }
    }
    if (register_locked_address != 0) {
        if (install_hook(reinterpret_cast<void *>(register_locked_address),
                         reinterpret_cast<void *>(dejavu_unpack_register_dex_file_locked),
                         &g_register_dexfile_locked_original, &hook_error)) {
            register_dexfile_locked_address_ = register_locked_address;
            LOGD("ART RegisterDexFileLocked hook installed: %s @0x%llx",
                 register_locked_name.c_str(), register_locked_address);
        } else {
            LOGE("RegisterDexFileLocked hook failed: %s", hook_error.c_str());
        }
    }
    if (opencommon_address != 0) {
        if (install_hook(reinterpret_cast<void *>(opencommon_address),
                         reinterpret_cast<void *>(dejavu_unpack_open_common),
                         &g_opencommon_original, &hook_error)) {
            opencommon_address_ = opencommon_address;
            LOGD("ART OpenCommon hook installed: %s @0x%llx", opencommon_name.c_str(),
                 opencommon_address);
        } else {
            LOGE("OpenCommon hook failed: %s", hook_error.c_str());
        }
    }
    art_hooked_ = true;
    if (register_address == 0 && register_locked_address == 0 && opencommon_address == 0) {
        *error = "no RegisterDexFile/RegisterDexFileLocked/OpenCommon symbols found in libart.so";
        return false;
    }
    return true;
}

void UnpackManager::on_library_loaded(const char *filename) {
    if (g_inside_dlopen_hook) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++dlopen_events_;
    }
    if (filename == nullptr || !enabled_.load()) {
        return;
    }
    const std::string name(filename);
    if (name.find("libart") == std::string::npos) {
        return;
    }
    std::string error;
    if (!install_art_hooks(&error)) {
        LOGE("late libart hook installation failed: %s", error.c_str());
    }
}

void UnpackManager::on_register_dex_file(void *dex_file) {
    if (!enabled_.load() || dex_file == nullptr) {
        return;
    }
    const uintptr_t object = normalize_address(reinterpret_cast<uintptr_t>(dex_file));
    for (size_t offset = 0; offset <= 0x20; offset += sizeof(uintptr_t)) {
        uintptr_t begin = 0;
        size_t size = 0;
        if (!safe_read(object + offset, &begin, sizeof(begin)) ||
            !safe_read(object + offset + sizeof(uintptr_t), &size, sizeof(size))) {
            continue;
        }
        if (begin < 0x1000 || size < 0x70 || size > kMaximumDumpSize) {
            continue;
        }
        unsigned char header[0x40] = {};
        if (!safe_read(begin, header, sizeof(header))) {
            continue;
        }
        size_t image_size = 0;
        if (!looks_like_dex(header, sizeof(header), &image_size)) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            bool known = false;
            for (const Image &image : images_) {
                if (image.begin == begin) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                images_.push_back(Image{begin, image_size});
            }
        }
        if (!include_file_backed_.load() && is_file_backed(begin)) {
            ++skipped_file_backed_;
            continue;
        }
        if (enqueue_dex(begin, image_size, "register_dexfile")) {
            return;
        }
    }
}

void UnpackManager::try_dump_pointer(unsigned long long value, const char *label) {
    if (!enabled_.load() || value < 0x1000) {
        return;
    }
    unsigned char header[0x40] = {};
    if (!safe_read(static_cast<uintptr_t>(normalize_address(value)), header, sizeof(header))) {
        return;
    }
    size_t image_size = 0;
    if (!looks_like_dex(header, sizeof(header), &image_size)) {
        return;
    }
    const uintptr_t pointer = normalize_address(static_cast<uintptr_t>(value));
    if (!include_file_backed_.load() && is_file_backed(pointer)) {
        ++skipped_file_backed_;
        return;
    }
    enqueue_dex(pointer, image_size, label);
}

bool UnpackManager::dump_dex(unsigned long long address, size_t size, const char *label) {
    address = static_cast<unsigned long long>(normalize_address(static_cast<uintptr_t>(address)));
    if (address == 0 || size < 0x70 || size > kMaximumDumpSize) {
        return false;
    }
    unsigned char header[0x40] = {};
    if (!safe_read(static_cast<uintptr_t>(address), header, sizeof(header))) {
        return false;
    }
    size_t image_size = 0;
    if (!looks_like_dex(header, sizeof(header), &image_size)) {
        return false;
    }
    if (size < image_size) {
        image_size = size;
    }
    std::string path;
    std::string error;
    if (!dump_bytes(address, image_size, label, true, &path, &error)) {
        if (error.find("already dumped") == std::string::npos) {
            LOGE("dex dump failed at 0x%llx: %s", address, error.c_str());
        }
        return false;
    }
    LOGD("dumped dex from 0x%llx size=%zu to %s", address, image_size, path.c_str());
    return true;
}

bool UnpackManager::dump_bytes(unsigned long long address, size_t size, const char *label,
                               bool require_dex, std::string *path, std::string *error) {
    if (size == 0 || size > kMaximumDumpSize) {
        *error = "invalid dump size";
        return false;
    }
    address = static_cast<unsigned long long>(normalize_address(static_cast<uintptr_t>(address)));
    std::vector<unsigned char> buffer(size);
    if (!safe_read(static_cast<uintptr_t>(address), buffer.data(), size)) {
        *error = "unable to read dump range";
        return false;
    }
    if (require_dex) {
        size_t image_size = 0;
        if (!looks_like_dex(buffer.data(), buffer.size(), &image_size)) {
            *error = "buffer is not a recognized Dex/CompactDex image";
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string directory = directory_;
    if (directory.empty()) {
        *error = "unpack output directory is not configured";
        return false;
    }
    char key[96];
    snprintf(key, sizeof(key), "%s:%zu:%02x%02x%02x%02x%02x%02x%02x%02x",
             label != nullptr ? label : "memory", size, buffer.size() > 12 ? buffer[12] : 0,
             buffer.size() > 13 ? buffer[13] : 0, buffer.size() > 14 ? buffer[14] : 0,
             buffer.size() > 15 ? buffer[15] : 0, buffer.size() > 16 ? buffer[16] : 0,
             buffer.size() > 17 ? buffer[17] : 0, buffer.size() > 18 ? buffer[18] : 0,
             buffer.size() > 19 ? buffer[19] : 0);
    if (dumped_keys_.find(key) != dumped_keys_.end()) {
        *error = "identical image was already dumped";
        return false;
    }
    const unsigned long long sequence = sequence_++;
    const bool is_cdex = buffer.size() >= 7 && memcmp(buffer.data(), kCdexMagic, 7) == 0;
    const char *extension = is_cdex ? "cdex" : "dex";
    char filename[256];
    snprintf(filename, sizeof(filename), "%s/%s_%03llu.%s", directory.c_str(),
             label != nullptr ? label : "memory", sequence, require_dex ? extension : "bin");
    const int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        *error = std::string("unable to create dump file: ") + strerror(errno);
        return false;
    }
    size_t offset = 0;
    while (offset < buffer.size()) {
        const ssize_t written = write(fd, buffer.data() + offset, buffer.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            close(fd);
            *error = std::string("unable to write dump file: ") + strerror(errno);
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    close(fd);
    dumped_keys_.insert(key);
    ++dumped_count_;
    *path = filename;
    return true;
}

std::string UnpackManager::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string output;
    output += enabled_.load() ? "enabled=yes" : "enabled=no";
    output += " dir=" + directory_;
    output += " dlopen_hook=" + std::string(dlopen_hooked_ ? "yes" : "no");
    output += " dlopen_events=" + std::to_string(dlopen_events_);
    output += " art_hooked=" + std::string(art_hooked_ ? "yes" : "no");
    char buffer[96];
    snprintf(buffer, sizeof(buffer), " register_dexfile=0x%llx",
             static_cast<unsigned long long>(register_dexfile_address_));
    output += buffer;
    snprintf(buffer, sizeof(buffer), " register_dexfile_locked=0x%llx",
             static_cast<unsigned long long>(register_dexfile_locked_address_));
    output += buffer;
    snprintf(buffer, sizeof(buffer), " opencommon=0x%llx",
             static_cast<unsigned long long>(opencommon_address_));
    output += buffer;
    output += " dumped=" + std::to_string(dumped_count_ + written_files_);
    output += " skipped_file_backed=" + std::to_string(skipped_file_backed_);
    output += " include_file_backed=" + std::string(include_file_backed_.load() ? "yes" : "no");
    output += " invoke_hooked=" + std::string(invoke_hooked_ ? "yes" : "no");
    char invoke_buffer[96];
    snprintf(invoke_buffer, sizeof(invoke_buffer), " invoke=0x%lx",
             static_cast<unsigned long>(invoke_address_));
    output += invoke_buffer;
    output += " active_invoke_hits=" + std::to_string(active_invoke_hits_.load());
    output += " revision_dumps=" + std::to_string(revision_dumps_);
    {
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);
        output += " pending_files=" + std::to_string(queue_.size());
        output += " pending_bytes=" + std::to_string(pending_bytes_);
    }
    output += " written_files=" + std::to_string(written_files_);
    output += " dropped_budget=" + std::to_string(dropped_budget_);
    output += " active_running=" + std::string(active_running_.load() ? "yes" : "no");
    {
        std::lock_guard<std::mutex> active_lock(active_mutex_);
        output += " active_done=" + std::to_string(active_done_);
        output += " active_total=" + std::to_string(active_total_);
        output += " active_errors=" + std::to_string(active_errors_.load());
    }
    return output;
}

extern "C" void *dejavu_unpack_dlopen(const char *filename, int flags) {
    using Function = void *(*)(const char *, int);
    g_inside_dlopen_hook = true;
    void *result = reinterpret_cast<Function>(g_dlopen_original)(filename, flags);
    g_inside_dlopen_hook = false;
    if (g_unpack != nullptr) {
        g_unpack->on_library_loaded(filename);
    }
    return result;
}

extern "C" void *dejavu_unpack_android_dlopen_ext(const char *filename, int flags,
                                                  const void *info) {
    using Function = void *(*)(const char *, int, const void *);
    g_inside_dlopen_hook = true;
    void *result = reinterpret_cast<Function>(g_android_dlopen_ext_original)(filename, flags, info);
    g_inside_dlopen_hook = false;
    if (g_unpack != nullptr) {
        g_unpack->on_library_loaded(filename);
    }
    return result;
}

extern "C" void dejavu_unpack_register_dex_file(void *self, void *dex_file, void *loader) {
    if (g_unpack != nullptr) {
        g_unpack->on_register_dex_file(dex_file);
    }
    using Function = void (*)(void *, void *, void *);
    reinterpret_cast<Function>(g_register_dexfile_original)(self, dex_file, loader);
}

extern "C" void dejavu_unpack_register_dex_file_locked(void *self, void *dex_file, void *dex_cache,
                                                       void *loader) {
    if (g_unpack != nullptr) {
        g_unpack->on_register_dex_file(dex_file);
    }
    using Function = void (*)(void *, void *, void *, void *);
    reinterpret_cast<Function>(g_register_dexfile_locked_original)(self, dex_file, dex_cache,
                                                                   loader);
}

extern "C" unsigned long dejavu_unpack_open_common(unsigned long a0, unsigned long a1,
                                                   unsigned long a2, unsigned long a3,
                                                   unsigned long a4, unsigned long a5,
                                                   unsigned long a6, unsigned long a7) {
    if (g_unpack != nullptr) {
        g_unpack->try_dump_pointer(a0, "open_common");
        g_unpack->try_dump_pointer(a1, "open_common");
        g_unpack->try_dump_pointer(a2, "open_common");
        g_unpack->try_dump_pointer(a3, "open_common");
        g_unpack->try_dump_pointer(a4, "open_common");
        g_unpack->try_dump_pointer(a5, "open_common");
        g_unpack->try_dump_pointer(a6, "open_common");
        g_unpack->try_dump_pointer(a7, "open_common");
    }
    using Function = unsigned long (*)(unsigned long, unsigned long, unsigned long, unsigned long,
                                       unsigned long, unsigned long, unsigned long, unsigned long);
    return reinterpret_cast<Function>(g_opencommon_original)(a0, a1, a2, a3, a4, a5, a6, a7);
}

int UnpackManager::lua_enable(dejavu_control_session *, void *context,
                              const dejavu_value *arguments, size_t argument_count,
                              dejavu_value *result, char *error, size_t error_size) {
    auto *manager = static_cast<UnpackManager *>(context);
    if (manager == nullptr || argument_count != 1 || arguments[0].type != DEJAVU_VALUE_STRING) {
        snprintf(error, error_size, "unpack.enable expects an output directory");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::string message;
    if (!manager->enable(arguments[0].string_value == nullptr
                             ? std::string()
                             : std::string(arguments[0].string_value, arguments[0].string_size),
                         &message)) {
        snprintf(error, error_size, "%s", message.c_str());
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    const std::string text = manager->status();
    char *copy = static_cast<char *>(malloc(text.size() + 1));
    if (copy == nullptr) {
        snprintf(error, error_size, "unable to allocate unpack status");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    memcpy(copy, text.data(), text.size());
    copy[text.size()] = '\0';
    result->type = DEJAVU_VALUE_STRING;
    result->owned = 1;
    result->string_value = copy;
    result->string_size = text.size();
    return DEJAVU_OK;
}

int UnpackManager::lua_disable(dejavu_control_session *, void *context, const dejavu_value *,
                               size_t argument_count, dejavu_value *result, char *error,
                               size_t error_size) {
    auto *manager = static_cast<UnpackManager *>(context);
    if (manager == nullptr || argument_count != 0) {
        snprintf(error, error_size, "unpack.disable takes no arguments");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    manager->enabled_.store(false);
    result->type = DEJAVU_VALUE_BOOLEAN;
    result->boolean_value = 1;
    return DEJAVU_OK;
}

int UnpackManager::lua_status(dejavu_control_session *, void *context, const dejavu_value *,
                              size_t argument_count, dejavu_value *result, char *error,
                              size_t error_size) {
    auto *manager = static_cast<UnpackManager *>(context);
    if (manager == nullptr || argument_count != 0) {
        snprintf(error, error_size, "unpack.status takes no arguments");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    const std::string text = manager->status();
    char *copy = static_cast<char *>(malloc(text.size() + 1));
    if (copy == nullptr) {
        snprintf(error, error_size, "unable to allocate unpack status");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    memcpy(copy, text.data(), text.size());
    copy[text.size()] = '\0';
    result->type = DEJAVU_VALUE_STRING;
    result->owned = 1;
    result->string_value = copy;
    result->string_size = text.size();
    return DEJAVU_OK;
}

int UnpackManager::lua_install_art_hooks(dejavu_control_session *, void *context,
                                         const dejavu_value *, size_t argument_count,
                                         dejavu_value *result, char *error, size_t error_size) {
    auto *manager = static_cast<UnpackManager *>(context);
    if (manager == nullptr || argument_count != 0) {
        snprintf(error, error_size, "unpack.install_art_hooks takes no arguments");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::string message;
    if (!manager->install_art_hooks(&message)) {
        snprintf(error, error_size, "%s", message.c_str());
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    const std::string text = manager->status();
    char *copy = static_cast<char *>(malloc(text.size() + 1));
    if (copy == nullptr) {
        snprintf(error, error_size, "unable to allocate unpack status");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    memcpy(copy, text.data(), text.size());
    copy[text.size()] = '\0';
    result->type = DEJAVU_VALUE_STRING;
    result->owned = 1;
    result->string_value = copy;
    result->string_size = text.size();
    return DEJAVU_OK;
}

int UnpackManager::lua_dump_memory(dejavu_control_session *, void *context,
                                   const dejavu_value *arguments, size_t argument_count,
                                   dejavu_value *result, char *error, size_t error_size) {
    auto *manager = static_cast<UnpackManager *>(context);
    if (manager == nullptr || argument_count < 2 || argument_count > 3 ||
        arguments[0].type != DEJAVU_VALUE_INTEGER || arguments[1].type != DEJAVU_VALUE_INTEGER ||
        (argument_count == 3 && arguments[2].type != DEJAVU_VALUE_STRING)) {
        snprintf(error, error_size, "unpack.dump_memory expects address, size and optional label");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    if (arguments[0].integer_value <= 0 || arguments[1].integer_value <= 0) {
        snprintf(error, error_size, "unpack.dump_memory expects positive address and size");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    const std::string label = argument_count == 3
                                  ? std::string(arguments[2].string_value, arguments[2].string_size)
                                  : std::string("memory");
    std::string path;
    std::string message;
    if (!manager->dump_bytes(static_cast<unsigned long long>(arguments[0].integer_value),
                             static_cast<size_t>(arguments[1].integer_value), label.c_str(), false,
                             &path, &message)) {
        snprintf(error, error_size, "%s", message.c_str());
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    char *copy = static_cast<char *>(malloc(path.size() + 1));
    if (copy == nullptr) {
        snprintf(error, error_size, "unable to allocate dump path");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    memcpy(copy, path.data(), path.size());
    copy[path.size()] = '\0';
    result->type = DEJAVU_VALUE_STRING;
    result->owned = 1;
    result->string_value = copy;
    result->string_size = path.size();
    return DEJAVU_OK;
}

int UnpackManager::lua_include_file_backed(dejavu_control_session *, void *context,
                                           const dejavu_value *arguments, size_t argument_count,
                                           dejavu_value *result, char *error, size_t error_size) {
    auto *manager = static_cast<UnpackManager *>(context);
    if (manager == nullptr || argument_count != 1 || arguments[0].type != DEJAVU_VALUE_BOOLEAN) {
        snprintf(error, error_size, "unpack.include_file_backed expects a boolean");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    manager->include_file_backed_.store(arguments[0].boolean_value != 0);
    result->type = DEJAVU_VALUE_BOOLEAN;
    result->boolean_value = 1;
    return DEJAVU_OK;
}

JNIEnv *UnpackManager::current_env() {
    if (vm_ == nullptr) {
        return nullptr;
    }
    JNIEnv *env = nullptr;
    const jint status = vm_->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (status == JNI_OK) {
        return env;
    }
    if (status == JNI_EDETACHED && vm_->AttachCurrentThread(&env, nullptr) == JNI_OK) {
        return env;
    }
    return nullptr;
}

void UnpackManager::on_active_invoke() {
    active_invoke_hits_.fetch_add(1);
}

jobject UnpackManager::current_app_class_loader() {
    if (host_ != nullptr && host_->get_app_class_loader != nullptr) {
        return host_->get_app_class_loader(host_->host_context);
    }
    return nullptr;
}

bool UnpackManager::hook_invoke(std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (invoke_hooked_) {
        return true;
    }
    ArtElfResolver resolver;
    std::string resolver_error;
    if (!resolver.open_loaded("libart.so", &resolver_error)) {
        *error = "unable to inspect libart.so: " + resolver_error;
        return false;
    }
    std::vector<ArtElfSymbol> symbols;
    if (!resolver.enumerate_symbols(&symbols, &resolver_error)) {
        *error = "unable to enumerate libart symbols: " + resolver_error;
        return false;
    }
    unsigned long long art_method_address = 0;
    std::string art_method_name;
    for (const ArtElfSymbol &symbol : symbols) {
        if (symbol.name.find("ArtMethod6Invoke") != std::string::npos) {
            art_method_address = symbol.address;
            art_method_name = symbol.name;
            break;
        }
    }
    std::string hook_error;
    bool installed = false;
    if (art_method_address != 0) {
        if (install_hook(reinterpret_cast<void *>(art_method_address),
                         reinterpret_cast<void *>(dejavu_unpack_art_method_invoke),
                         &g_art_method_invoke_original, &hook_error)) {
            g_art_method_invoke_target = reinterpret_cast<void *>(art_method_address);
            invoke_address_ = static_cast<unsigned long>(art_method_address);
            invoke_hooked_ = true;
            LOGD("ArtMethod::Invoke hook installed: %s @0x%llx", art_method_name.c_str(),
                 art_method_address);
            return true;
        }
        LOGE("ArtMethod::Invoke hook failed: %s", hook_error.c_str());
    }
    unsigned long long jvalues_address = 0;
    unsigned long long varargs_address = 0;
    unsigned long long argarray_address = 0;
    std::string varargs_name;
    std::string jvalues_name;
    std::string argarray_name;
    for (const ArtElfSymbol &symbol : symbols) {
        if (jvalues_address == 0 && symbol.name.find("InvokeWithJValues") != std::string::npos) {
            jvalues_address = symbol.address;
            jvalues_name = symbol.name;
        }
        if (varargs_address == 0 && symbol.name.find("InvokeWithVarArgs") != std::string::npos) {
            varargs_address = symbol.address;
            varargs_name = symbol.name;
        }
        if (argarray_address == 0 && symbol.name.find("InvokeWithArgArray") != std::string::npos) {
            argarray_address = symbol.address;
            argarray_name = symbol.name;
        }
    }
    if (jvalues_address != 0) {
        if (install_hook(reinterpret_cast<void *>(jvalues_address),
                         reinterpret_cast<void *>(dejavu_unpack_invoke_with_jvalues),
                         &g_invoke_with_jvalues_original, &hook_error)) {
            invoke_address_ = static_cast<unsigned long>(jvalues_address);
            installed = true;
            LOGD("InvokeWithJValues hook installed: %s @0x%llx", jvalues_name.c_str(),
                 jvalues_address);
        } else {
            LOGE("InvokeWithJValues hook failed: %s", hook_error.c_str());
        }
    }
    if (varargs_address != 0) {
        if (install_hook(reinterpret_cast<void *>(varargs_address),
                         reinterpret_cast<void *>(dejavu_unpack_invoke_with_varargs),
                         &g_invoke_with_varargs_original, &hook_error)) {
            if (!installed) {
                invoke_address_ = static_cast<unsigned long>(varargs_address);
                installed = true;
            }
            LOGD("InvokeWithVarArgs hook installed: %s @0x%llx", varargs_name.c_str(),
                 varargs_address);
        } else {
            LOGE("InvokeWithVarArgs hook failed: %s", hook_error.c_str());
        }
    }
    if (argarray_address != 0) {
        if (install_hook(reinterpret_cast<void *>(argarray_address),
                         reinterpret_cast<void *>(dejavu_unpack_invoke_with_arg_array),
                         &g_invoke_with_arg_array_original, &hook_error)) {
            if (!installed) {
                invoke_address_ = static_cast<unsigned long>(argarray_address);
                installed = true;
            }
            LOGD("InvokeWithArgArray hook installed: %s @0x%llx", argarray_name.c_str(),
                 argarray_address);
        } else {
            LOGE("InvokeWithArgArray hook failed: %s", hook_error.c_str());
        }
    }
    if (!installed) {
        *error = "no InvokeWithJValues/InvokeWithArgArray symbol found in libart.so";
        return false;
    }
    invoke_hooked_ = true;
    return true;
}

bool UnpackManager::dump_discovered_revision(std::string *error) {
    std::vector<Image> images;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        images = images_;
    }
    if (images.empty()) {
        *error = "no DexFile images were observed yet";
        return false;
    }
    bool dumped_any = false;
    for (const Image &image : images) {
        if (!include_file_backed_.load() && is_file_backed(image.begin)) {
            continue;
        }
        if (enqueue_dex(image.begin, image.size, "active_revision")) {
            ++revision_dumps_;
            dumped_any = true;
        }
    }
    if (!dumped_any) {
        *error = "no new revision was produced";
        return false;
    }
    return true;
}

bool UnpackManager::active_invoke(const std::string &class_names, std::string *stats,
                                  std::string *error) {
    if (!invoke_hooked_ || g_art_method_invoke_target == nullptr) {
        *error = "call unpack.hook_invoke() (ArtMethod::Invoke) before active_invoke()";
        return false;
    }
    std::vector<std::string> names;
    std::string current;
    for (char character : class_names) {
        if (character == ',' || character == ';' || character == '\n' || character == ' ') {
            if (!current.empty()) {
                names.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(character);
        }
    }
    if (!current.empty()) {
        names.push_back(current);
    }
    if (names.empty()) {
        *error = "unpack.active_invoke expects a comma-separated class name list";
        return false;
    }
    JNIEnv *env = current_env();
    if (env == nullptr) {
        *error = "Java thread is not attached";
        return false;
    }
    jclass class_class = env->FindClass("java/lang/Class");
    jclass method_class = env->FindClass("java/lang/reflect/Method");
    if (class_class == nullptr || method_class == nullptr) {
        env->ExceptionClear();
        *error = "unable to resolve reflection classes";
        return false;
    }
    const jmethodID class_for_name = env->GetStaticMethodID(
        class_class, "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
    const jmethodID class_get_methods =
        env->GetMethodID(class_class, "getDeclaredMethods", "()[Ljava/lang/reflect/Method;");
    if (class_for_name == nullptr || class_get_methods == nullptr) {
        env->ExceptionClear();
        *error = "unable to resolve reflection methods";
        return false;
    }
    using InvokeFunction = void (*)(void *, void *, uint32_t *, uint32_t, void *, const char *);
    auto invoke = reinterpret_cast<InvokeFunction>(g_art_method_invoke_target);
    unsigned long long found_classes = 0;
    unsigned long long invoked = 0;
    unsigned long long skipped_methods = 0;
    unsigned long long errors = 0;
    for (const std::string &name : names) {
        jstring class_name = env->NewStringUTF(name.c_str());
        if (class_name == nullptr) {
            env->ExceptionClear();
            continue;
        }
        jclass clazz = static_cast<jclass>(env->CallStaticObjectMethod(
            class_class, class_for_name, class_name, JNI_FALSE, current_app_class_loader()));
        env->DeleteLocalRef(class_name);
        if (env->ExceptionCheck() || clazz == nullptr) {
            env->ExceptionClear();
            continue;
        }
        ++found_classes;
        jobjectArray methods =
            static_cast<jobjectArray>(env->CallObjectMethod(clazz, class_get_methods));
        if (env->ExceptionCheck() || methods == nullptr) {
            env->ExceptionClear();
            env->DeleteLocalRef(clazz);
            continue;
        }
        const jsize method_count = env->GetArrayLength(methods);
        for (jsize method_index = 0; method_index < method_count; ++method_index) {
            jobject method = env->GetObjectArrayElement(methods, method_index);
            if (method == nullptr) {
                continue;
            }
            jmethodID method_id = env->FromReflectedMethod(method);
            if (method_id == nullptr) {
                ++skipped_methods;
                env->DeleteLocalRef(method);
                continue;
            }
            // Sentinel call: thread == nullptr is intercepted by the ArtMethod::Invoke hook.
            jvalue result_value = {};
            invoke(method_id, nullptr, nullptr, 0, &result_value, nullptr);
            ++invoked;
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                ++errors;
            }
            env->DeleteLocalRef(method);
        }
        env->DeleteLocalRef(methods);
        env->DeleteLocalRef(clazz);
    }
    std::string revision_error;
    const bool dumped = dump_discovered_revision(&revision_error);
    env->DeleteLocalRef(method_class);
    env->DeleteLocalRef(class_class);
    char summary[512];
    snprintf(summary, sizeof(summary),
             "requested_classes=%zu found_classes=%llu sentinel_invoke_calls=%llu "
             "skipped_methods=%llu errors=%llu sentinel_hits=%llu revision_dumps=%llu "
             "revision_dump_ok=%d",
             names.size(), found_classes, invoked, skipped_methods, errors,
             active_invoke_hits_.load(), revision_dumps_, dumped ? 1 : 0);
    *stats = summary;
    return true;
}

extern "C" void dejavu_unpack_art_method_invoke(void *method, void *thread, uint32_t *args,
                                                uint32_t args_size, void *result,
                                                const char *shorty) {
    if (thread == nullptr) {
        if (g_unpack != nullptr) {
            g_unpack->on_active_invoke();
        }
        if (result != nullptr) {
            memset(result, 0, sizeof(jvalue));
        }
        return;
    }
    using Function = void (*)(void *, void *, uint32_t *, uint32_t, void *, const char *);
    reinterpret_cast<Function>(g_art_method_invoke_original)(method, thread, args, args_size,
                                                             result, shorty);
}

extern "C" jvalue dejavu_unpack_invoke_with_jvalues(void *soa, jobject receiver, void *method,
                                                    const jvalue *arguments) {
    if (g_active_invoke) {
        if (g_unpack != nullptr) {
            g_unpack->on_active_invoke();
        }
        jvalue zero = {};
        return zero;
    }
    using Function = jvalue (*)(void *, jobject, void *, const jvalue *);
    return reinterpret_cast<Function>(g_invoke_with_jvalues_original)(soa, receiver, method,
                                                                      arguments);
}

extern "C" jvalue dejavu_unpack_invoke_with_varargs(void *soa, jobject receiver, void *method,
                                                    const void *arguments) {
    if (g_active_invoke) {
        if (g_unpack != nullptr) {
            g_unpack->on_active_invoke();
        }
        jvalue zero = {};
        return zero;
    }
    using Function = jvalue (*)(void *, jobject, void *, const void *);
    return reinterpret_cast<Function>(g_invoke_with_varargs_original)(soa, receiver, method,
                                                                      arguments);
}

extern "C" void dejavu_unpack_invoke_with_arg_array(void *soa, void *method, void *argument_array,
                                                    void *result, const char *error_message) {
    if (g_active_invoke) {
        if (g_unpack != nullptr) {
            g_unpack->on_active_invoke();
        }
        if (result != nullptr) {
            memset(result, 0, sizeof(jvalue));
        }
        return;
    }
    using Function = void (*)(void *, void *, void *, void *, const char *);
    reinterpret_cast<Function>(g_invoke_with_arg_array_original)(soa, method, argument_array,
                                                                 result, error_message);
}

int UnpackManager::lua_hook_invoke(dejavu_control_session *, void *context, const dejavu_value *,
                                   size_t argument_count, dejavu_value *result, char *error,
                                   size_t error_size) {
    auto *manager = static_cast<UnpackManager *>(context);
    if (manager == nullptr || argument_count != 0) {
        snprintf(error, error_size, "unpack.hook_invoke takes no arguments");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::string message;
    if (!manager->hook_invoke(&message)) {
        snprintf(error, error_size, "%s", message.c_str());
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    const std::string text = manager->status();
    char *copy = static_cast<char *>(malloc(text.size() + 1));
    if (copy == nullptr) {
        snprintf(error, error_size, "unable to allocate status");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    memcpy(copy, text.data(), text.size());
    copy[text.size()] = '\0';
    result->type = DEJAVU_VALUE_STRING;
    result->owned = 1;
    result->string_value = copy;
    result->string_size = text.size();
    return DEJAVU_OK;
}

int UnpackManager::lua_active_invoke(dejavu_control_session *, void *context,
                                     const dejavu_value *arguments, size_t argument_count,
                                     dejavu_value *result, char *error, size_t error_size) {
    auto *manager = static_cast<UnpackManager *>(context);
    if (manager == nullptr || argument_count > 1 ||
        (argument_count == 1 && arguments[0].type != DEJAVU_VALUE_STRING)) {
        snprintf(error, error_size, "unpack.active_invoke expects an optional class filter");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::string filter;
    if (argument_count == 1 && arguments[0].string_value != nullptr) {
        filter.assign(arguments[0].string_value, arguments[0].string_size);
    }
    std::string stats;
    std::string message;
    if (!manager->active_invoke(filter, &stats, &message)) {
        snprintf(error, error_size, "%s", message.c_str());
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    const std::string text = manager->status() + "\n" + stats;
    char *copy = static_cast<char *>(malloc(text.size() + 1));
    if (copy == nullptr) {
        snprintf(error, error_size, "unable to allocate active invoke status");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    memcpy(copy, text.data(), text.size());
    copy[text.size()] = '\0';
    result->type = DEJAVU_VALUE_STRING;
    result->owned = 1;
    result->string_value = copy;
    result->string_size = text.size();
    return DEJAVU_OK;
}

void UnpackManager::start_writer() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (writer_.joinable()) {
        return;
    }
    writer_stop_ = false;
    writer_ = std::thread([this]() { writer_loop(); });
}

void UnpackManager::stop_writer() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        writer_stop_ = true;
    }
    queue_cv_.notify_all();
    if (writer_.joinable()) {
        writer_.join();
    }
}

void UnpackManager::writer_loop() {
    for (;;) {
        PendingDump pending;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this]() { return writer_stop_ || !queue_.empty(); });
            if (queue_.empty() && writer_stop_) {
                break;
            }
            if (queue_.empty()) {
                continue;
            }
            pending = std::move(queue_.front());
            queue_.pop_front();
            pending_bytes_ -= pending.data.size();
            writer_writing_ = true;
        }
        std::string directory;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            directory = directory_;
        }
        bool ok = false;
        if (!directory.empty()) {
            const unsigned long long sequence = sequence_++;
            char filename[256];
            snprintf(filename, sizeof(filename), "%s/%s_%03llu.%s", directory.c_str(),
                     pending.label.c_str(), sequence, pending.extension.c_str());
            const int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd >= 0) {
                size_t offset = 0;
                ok = true;
                while (offset < pending.data.size()) {
                    const ssize_t written =
                        write(fd, pending.data.data() + offset, pending.data.size() - offset);
                    if (written < 0 && errno == EINTR) {
                        continue;
                    }
                    if (written <= 0) {
                        ok = false;
                        break;
                    }
                    offset += static_cast<size_t>(written);
                }
                close(fd);
                if (!ok) {
                    LOGE("deferred dump write failed for %s", filename);
                }
            } else {
                LOGE("unable to create deferred dump %s: %s", filename, strerror(errno));
            }
        }
        if (ok) {
            ++written_files_;
        }
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            writer_writing_ = false;
        }
        queue_cv_.notify_all();
        if (!pending.data.empty()) {
            usleep(20 * 1000);
        }
    }
}

bool UnpackManager::enqueue_dex(unsigned long long address, size_t size, const char *label) {
    if (address == 0 || size < 0x70 || size > kMaximumDumpSize) {
        return false;
    }
    const uintptr_t pointer = normalize_address(static_cast<uintptr_t>(address));
    unsigned char header[0x40] = {};
    if (!safe_read(pointer, header, sizeof(header))) {
        return false;
    }
    size_t image_size = 0;
    if (!looks_like_dex(header, sizeof(header), &image_size)) {
        return false;
    }
    if (size < image_size) {
        image_size = size;
    }
    std::vector<unsigned char> buffer(image_size);
    if (!safe_read(pointer, buffer.data(), buffer.size())) {
        return false;
    }
    if (!looks_like_dex(buffer.data(), buffer.size(), &image_size)) {
        return false;
    }
    char key[96];
    snprintf(key, sizeof(key), "%s:%zu:%02x%02x%02x%02x%02x%02x%02x%02x",
             label != nullptr ? label : "memory", buffer.size(),
             buffer.size() > 12 ? buffer[12] : 0, buffer.size() > 13 ? buffer[13] : 0,
             buffer.size() > 14 ? buffer[14] : 0, buffer.size() > 15 ? buffer[15] : 0,
             buffer.size() > 16 ? buffer[16] : 0, buffer.size() > 17 ? buffer[17] : 0,
             buffer.size() > 18 ? buffer[18] : 0, buffer.size() > 19 ? buffer[19] : 0);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!dumped_keys_.insert(key).second || directory_.empty()) {
            return false;
        }
    }
    PendingDump pending;
    pending.label = label != nullptr ? label : "memory";
    pending.extension =
        buffer.size() >= 7 && memcmp(buffer.data(), kCdexMagic, 7) == 0 ? "cdex" : "dex";
    pending.data = std::move(buffer);
    const size_t pending_size = pending.data.size();
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (pending_bytes_ + pending_size > max_pending_bytes_) {
            ++dropped_budget_;
            return false;
        }
        pending_bytes_ += pending_size;
        queue_.push_back(std::move(pending));
    }
    queue_cv_.notify_one();
    return true;
}

bool UnpackManager::flush(unsigned long long timeout_ms, std::string *error) {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    const bool done = queue_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                         [this]() { return queue_.empty() && !writer_writing_; });
    if (!done) {
        *error = "timed out waiting for deferred dumps to be written";
        return false;
    }
    return true;
}

int UnpackManager::lua_flush(dejavu_control_session *, void *context, const dejavu_value *arguments,
                             size_t argument_count, dejavu_value *result, char *error,
                             size_t error_size) {
    auto *manager = static_cast<UnpackManager *>(context);
    if (manager == nullptr || argument_count > 1 ||
        (argument_count == 1 && arguments[0].type != DEJAVU_VALUE_INTEGER)) {
        snprintf(error, error_size, "unpack.flush expects an optional timeout in milliseconds");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    unsigned long long timeout = 120000;
    if (argument_count == 1 && arguments[0].integer_value > 0) {
        timeout = static_cast<unsigned long long>(arguments[0].integer_value);
    }
    std::string message;
    if (!manager->flush(timeout, &message)) {
        snprintf(error, error_size, "%s", message.c_str());
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    const std::string text = manager->status();
    char *copy = static_cast<char *>(malloc(text.size() + 1));
    if (copy == nullptr) {
        snprintf(error, error_size, "unable to allocate flush status");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    memcpy(copy, text.data(), text.size());
    copy[text.size()] = '\0';
    result->type = DEJAVU_VALUE_STRING;
    result->owned = 1;
    result->string_value = copy;
    result->string_size = text.size();
    return DEJAVU_OK;
}

bool UnpackManager::start_active_invoke(const std::string &class_names, unsigned long long delay_ms,
                                        std::string *status, std::string *error) {
    if (active_running_.load()) {
        *error = "active invocation is already running";
        return false;
    }
    if (!invoke_hooked_ || g_art_method_invoke_target == nullptr) {
        *error = "call unpack.hook_invoke() before active invocation";
        return false;
    }
    std::vector<std::string> names;
    std::string current;
    for (char character : class_names) {
        if (character == ',' || character == ';' || character == '\n' || character == ' ') {
            if (!current.empty()) {
                names.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(character);
        }
    }
    if (!current.empty()) {
        names.push_back(current);
    }
    if (names.empty()) {
        *error = "active invocation needs a comma-separated class list";
        return false;
    }
    if (active_thread_.joinable()) {
        active_thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(active_mutex_);
        active_names_ = std::move(names);
        active_total_ = active_names_.size();
        active_done_ = 0;
        active_errors_.store(0);
        active_error_.clear();
    }
    active_delay_ms_ = delay_ms == 0 ? 40 : delay_ms;
    active_stop_.store(false);
    active_running_.store(true);
    active_thread_ = std::thread([this]() { active_invoke_worker(); });
    *status = this->status();
    return true;
}

void UnpackManager::active_invoke_worker() {
    JNIEnv *env = nullptr;
    if (vm_ == nullptr || vm_->AttachCurrentThread(&env, nullptr) != JNI_OK || env == nullptr) {
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            active_error_ = "unable to attach the active invocation thread";
        }
        active_running_.store(false);
        active_cv_.notify_all();
        return;
    }
    jclass class_class = env->FindClass("java/lang/Class");
    jclass method_class = env->FindClass("java/lang/reflect/Method");
    jmethodID class_for_name = nullptr;
    jmethodID class_get_methods = nullptr;
    if (class_class != nullptr && method_class != nullptr) {
        class_for_name =
            env->GetStaticMethodID(class_class, "forName",
                                   "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
        class_get_methods =
            env->GetMethodID(class_class, "getDeclaredMethods", "()[Ljava/lang/reflect/Method;");
    }
    if (class_for_name == nullptr || class_get_methods == nullptr) {
        env->ExceptionClear();
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            active_error_ = "unable to resolve reflection methods";
        }
        active_running_.store(false);
        active_cv_.notify_all();
        return;
    }
    using InvokeFunction = void (*)(void *, void *, uint32_t *, uint32_t, void *, const char *);
    auto invoke = reinterpret_cast<InvokeFunction>(g_art_method_invoke_target);
    for (;;) {
        std::string name;
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            if (active_stop_.load() || active_done_ >= active_names_.size()) {
                break;
            }
            name = active_names_[active_done_];
        }
        jstring class_name = env->NewStringUTF(name.c_str());
        if (class_name == nullptr) {
            env->ExceptionClear();
        } else {
            jclass clazz = static_cast<jclass>(env->CallStaticObjectMethod(
                class_class, class_for_name, class_name, JNI_FALSE, current_app_class_loader()));
            env->DeleteLocalRef(class_name);
            if (env->ExceptionCheck() || clazz == nullptr) {
                env->ExceptionClear();
                active_errors_.fetch_add(1);
            } else {
                jobjectArray methods =
                    static_cast<jobjectArray>(env->CallObjectMethod(clazz, class_get_methods));
                if (env->ExceptionCheck() || methods == nullptr) {
                    env->ExceptionClear();
                    active_errors_.fetch_add(1);
                } else {
                    const jsize method_count = env->GetArrayLength(methods);
                    for (jsize method_index = 0; method_index < method_count; ++method_index) {
                        jobject method = env->GetObjectArrayElement(methods, method_index);
                        if (method == nullptr) {
                            continue;
                        }
                        jmethodID method_id = env->FromReflectedMethod(method);
                        if (method_id != nullptr) {
                            jvalue result = {};
                            invoke(method_id, nullptr, nullptr, 0, &result, nullptr);
                        }
                        env->DeleteLocalRef(method);
                    }
                    env->DeleteLocalRef(methods);
                }
                env->DeleteLocalRef(clazz);
            }
        }
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            ++active_done_;
        }
        active_cv_.notify_all();
        if (active_delay_ms_ > 0) {
            usleep(static_cast<useconds_t>(active_delay_ms_ * 1000));
        }
    }
    std::string revision_error;
    dump_discovered_revision(&revision_error);
    {
        std::lock_guard<std::mutex> lock(active_mutex_);
        active_error_ = active_stop_.load() ? "stopped" : "completed";
    }
    active_running_.store(false);
    active_cv_.notify_all();
}

bool UnpackManager::wait_active_invoke(unsigned long long timeout_ms, std::string *status,
                                       std::string *error) {
    std::unique_lock<std::mutex> lock(active_mutex_);
    const bool done = active_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                          [this]() { return !active_running_.load(); });
    lock.unlock();
    if (!done) {
        *error = "timed out waiting for active invocation";
        return false;
    }
    if (active_thread_.joinable()) {
        active_thread_.join();
    }
    *status = this->status();
    return true;
}

void UnpackManager::stop_active_invoke() {
    active_stop_.store(true);
    active_cv_.notify_all();
}

int UnpackManager::lua_active_invoke_async(dejavu_control_session *, void *context,
                                           const dejavu_value *arguments, size_t argument_count,
                                           dejavu_value *result, char *error, size_t error_size) {
    auto *manager = static_cast<UnpackManager *>(context);
    if (manager == nullptr || argument_count < 1 || argument_count > 2 ||
        arguments[0].type != DEJAVU_VALUE_STRING ||
        (argument_count == 2 && arguments[1].type != DEJAVU_VALUE_INTEGER)) {
        snprintf(error, error_size,
                 "unpack.active_invoke_async expects classes and optional delay_ms");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::string names(arguments[0].string_value, arguments[0].string_size);
    const unsigned long long delay =
        argument_count == 2 && arguments[1].integer_value > 0
            ? static_cast<unsigned long long>(arguments[1].integer_value)
            : 40;
    std::string status;
    std::string message;
    if (!manager->start_active_invoke(names, delay, &status, &message)) {
        snprintf(error, error_size, "%s", message.c_str());
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    char *copy = static_cast<char *>(malloc(status.size() + 1));
    if (copy == nullptr) {
        snprintf(error, error_size, "unable to allocate status");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    memcpy(copy, status.data(), status.size());
    copy[status.size()] = '\0';
    result->type = DEJAVU_VALUE_STRING;
    result->owned = 1;
    result->string_value = copy;
    result->string_size = status.size();
    return DEJAVU_OK;
}

int UnpackManager::lua_active_invoke_wait(dejavu_control_session *, void *context,
                                          const dejavu_value *arguments, size_t argument_count,
                                          dejavu_value *result, char *error, size_t error_size) {
    auto *manager = static_cast<UnpackManager *>(context);
    if (manager == nullptr || argument_count > 1 ||
        (argument_count == 1 && arguments[0].type != DEJAVU_VALUE_INTEGER)) {
        snprintf(error, error_size, "unpack.active_invoke_wait expects an optional timeout");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    unsigned long long timeout = 300000;
    if (argument_count == 1 && arguments[0].integer_value > 0) {
        timeout = static_cast<unsigned long long>(arguments[0].integer_value);
    }
    std::string status;
    std::string message;
    if (!manager->wait_active_invoke(timeout, &status, &message)) {
        snprintf(error, error_size, "%s", message.c_str());
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    char *copy = static_cast<char *>(malloc(status.size() + 1));
    if (copy == nullptr) {
        snprintf(error, error_size, "unable to allocate status");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    memcpy(copy, status.data(), status.size());
    copy[status.size()] = '\0';
    result->type = DEJAVU_VALUE_STRING;
    result->owned = 1;
    result->string_value = copy;
    result->string_size = status.size();
    return DEJAVU_OK;
}

int UnpackManager::lua_active_invoke_status(dejavu_control_session *, void *context,
                                            const dejavu_value *, size_t argument_count,
                                            dejavu_value *result, char *error, size_t error_size) {
    auto *manager = static_cast<UnpackManager *>(context);
    if (manager == nullptr || argument_count != 0) {
        snprintf(error, error_size, "unpack.active_invoke_status takes no arguments");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    const std::string status = manager->status();
    char *copy = static_cast<char *>(malloc(status.size() + 1));
    if (copy == nullptr) {
        snprintf(error, error_size, "unable to allocate status");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    memcpy(copy, status.data(), status.size());
    copy[status.size()] = '\0';
    result->type = DEJAVU_VALUE_STRING;
    result->owned = 1;
    result->string_value = copy;
    result->string_size = status.size();
    return DEJAVU_OK;
}

int UnpackManager::lua_sleep_ms(dejavu_control_session *, void *, const dejavu_value *arguments,
                                size_t argument_count, dejavu_value *result, char *error,
                                size_t error_size) {
    if (argument_count != 1 || arguments[0].type != DEJAVU_VALUE_INTEGER ||
        arguments[0].integer_value < 0 || arguments[0].integer_value > 60000) {
        snprintf(error, error_size, "unpack.sleep_ms expects 0..60000 milliseconds");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    usleep(static_cast<useconds_t>(arguments[0].integer_value * 1000));
    result->type = DEJAVU_VALUE_BOOLEAN;
    result->boolean_value = 1;
    return DEJAVU_OK;
}

int UnpackManager::lua_dump_revision(dejavu_control_session *, void *context, const dejavu_value *,
                                     size_t argument_count, dejavu_value *result, char *error,
                                     size_t error_size) {
    auto *manager = static_cast<UnpackManager *>(context);
    if (manager == nullptr || argument_count != 0) {
        snprintf(error, error_size, "unpack.dump_revision takes no arguments");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::string message;
    if (!manager->dump_discovered_revision(&message)) {
        snprintf(error, error_size, "%s", message.c_str());
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    const std::string status = manager->status();
    char *copy = static_cast<char *>(malloc(status.size() + 1));
    if (copy == nullptr) {
        snprintf(error, error_size, "unable to allocate status");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    memcpy(copy, status.data(), status.size());
    copy[status.size()] = '\0';
    result->type = DEJAVU_VALUE_STRING;
    result->owned = 1;
    result->string_value = copy;
    result->string_size = status.size();
    return DEJAVU_OK;
}
