#include "unpack_manager.h"

#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <dobby.h>

#include "art_elf_resolver.h"

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
thread_local bool g_inside_dlopen_hook = false;

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

bool find_mapping(uintptr_t address, MemoryMapping *mapping, std::string *error) {
    address = normalize_address(address);
    std::vector<MemoryMapping> mappings;
    if (!read_mappings(&mappings, error)) {
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

bool hook_function(void *target, void *replacement, void **original, std::string *error) {
    int original_protection = 0;
    if (!page_permissions(target, &original_protection)) {
        *error = "unable to query target page permissions";
        return false;
    }
    if (!set_page_permissions(target, original_protection | PROT_WRITE | PROT_EXEC, error)) {
        return false;
    }
    const int status = DobbyHook(target, replacement, original);
    std::string restore_error;
    set_page_permissions(target, original_protection, &restore_error);
    if (status != 0) {
        *error = "DobbyHook failed with status " + std::to_string(status);
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

bool UnpackManager::initialize(DejavuRuntime *runtime, std::string *error) {
    if (runtime == nullptr) {
        *error = "invalid unpack manager initialization argument";
        return false;
    }
    runtime_ = runtime;
    instance_ = this;
    g_unpack = this;
    return true;
}

void UnpackManager::shutdown() {
    enabled_.store(false);
    if (g_unpack == this) {
        g_unpack = nullptr;
    }
    if (instance_ == this) {
        instance_ = nullptr;
    }
    runtime_ = nullptr;
}

bool UnpackManager::register_lua(dejavu_control_session *session, std::string *error) {
    if (runtime_ == nullptr || session == nullptr) {
        *error = "unpack manager is not initialized";
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
    };
    char runtime_error[512];
    for (const Registration &registration : registrations) {
        const int status = runtime_->control_session_register(session, "unpack", registration.name,
                                                              registration.function, this,
                                                              runtime_error, sizeof(runtime_error));
        if (status != DEJAVU_OK) {
            *error = runtime_error;
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
    if (!hook_function(dlopen_address, reinterpret_cast<void *>(dejavu_unpack_dlopen),
                       &g_dlopen_original, error)) {
        return false;
    }
    if (android_ext_address != nullptr && android_ext_address != dlopen_address) {
        std::string ext_error;
        if (!hook_function(android_ext_address,
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
        if (hook_function(reinterpret_cast<void *>(register_address),
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
        if (hook_function(reinterpret_cast<void *>(register_locked_address),
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
        if (hook_function(reinterpret_cast<void *>(opencommon_address),
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
        if (!include_file_backed_.load() && is_file_backed(begin)) {
            ++skipped_file_backed_;
            continue;
        }
        if (dump_dex(begin, image_size, "register_dexfile")) {
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
    dump_dex(pointer, image_size, label);
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
    output += " dumped=" + std::to_string(dumped_count_);
    output += " skipped_file_backed=" + std::to_string(skipped_file_backed_);
    output += " include_file_backed=" + std::string(include_file_backed_.load() ? "yes" : "no");
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
