#include "agent_helpers.h"

#include <dirent.h>
#include <elf.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "art_elf_resolver.h"

namespace {

constexpr size_t kMaximumReadSize = 64 * 1024 * 1024;
constexpr size_t kMaximumListOutput = 1024 * 1024;
constexpr size_t kMaximumSymbolOutput = 1024 * 1024;
constexpr size_t kMaximumSymbols = 50000;

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

std::string errno_message(const char *operation, const std::string &path) {
    return std::string(operation) + " failed for " + path + ": " + strerror(errno);
}

char symbol_type_letter(unsigned char type) {
    switch (type) {
    case STT_NOTYPE:
        return '?';
    case STT_OBJECT:
        return 'O';
    case STT_FUNC:
        return 'F';
    case STT_SECTION:
        return 'S';
    case STT_FILE:
        return 'f';
    case STT_COMMON:
        return 'C';
    case STT_TLS:
        return 'T';
    case STT_GNU_IFUNC:
        return 'I';
    default:
        return '?';
    }
}

const char *symbol_bind_name(unsigned char bind) {
    switch (bind) {
    case STB_LOCAL:
        return "LOCAL";
    case STB_GLOBAL:
        return "GLOBAL";
    case STB_WEAK:
        return "WEAK";
    case STB_GNU_UNIQUE:
        return "GNU_UNIQUE";
    default:
        return "?";
    }
}

std::string base_name(const std::string &path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

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
        unsigned long offset = 0;
        char device[16] = {};
        unsigned long inode = 0;
        int consumed = 0;
        if (sscanf(line.c_str(), "%lx-%lx %7s %lx %15s %lu %n", &start, &end, permissions, &offset,
                   device, &inode, &consumed) < 6) {
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
            mapping.path = line.substr(position);
        }
        mappings->push_back(std::move(mapping));
    }
    return true;
}

bool mapping_has_permission(const MemoryMapping &mapping, char permission) {
    if (mapping.permissions.size() < 3) {
        return false;
    }
    if (permission == 'r') {
        return mapping.permissions[0] == 'r';
    }
    if (permission == 'w') {
        return mapping.permissions[1] == 'w';
    }
    return mapping.permissions[2] == 'x';
}

bool address_from_value(const dejavu_value &value, uintptr_t *address, std::string *error) {
    if (value.type != DEJAVU_VALUE_INTEGER || value.integer_value <= 0) {
        *error = "address must be a positive integer";
        return false;
    }
    *address = static_cast<uintptr_t>(static_cast<unsigned long long>(value.integer_value));
    return true;
}

bool mapped_range(uintptr_t address, size_t size, char permission, MemoryMapping *mapping,
                  std::string *error) {
    if (size == 0) {
        *error = "size must be greater than zero";
        return false;
    }
    const uintptr_t last = address + size - 1;
    if (last < address) {
        *error = "address range overflows";
        return false;
    }
    std::vector<MemoryMapping> mappings;
    if (!read_mappings(&mappings, error)) {
        return false;
    }
    for (const MemoryMapping &candidate : mappings) {
        if (address >= candidate.start && last < candidate.end) {
            if (!mapping_has_permission(candidate, permission)) {
                *error = std::string("mapping is not ") + (permission == 'r'   ? "readable"
                                                           : permission == 'w' ? "writable"
                                                                               : "executable");
                return false;
            }
            if (mapping != nullptr) {
                *mapping = candidate;
            }
            return true;
        }
    }
    *error = "address range is not mapped";
    return false;
}

uint64_t load_unsigned(const void *source, size_t width) {
    uint64_t value = 0;
    memcpy(&value, source, width);
    return value;
}

void store_unsigned(void *destination, size_t width, uint64_t value) {
    memcpy(destination, &value, width);
}

bool parse_protection(const std::string &permissions, int *protection, std::string *error) {
    if (permissions.size() != 3) {
        *error = "permissions must be three characters such as 'rwx', 'rw-' or 'r-x'";
        return false;
    }
    int result = 0;
    for (size_t index = 0; index < 3; ++index) {
        const char character = permissions[index];
        if (character == 'r') {
            if (index != 0) {
                *error = "'r' must be the first permission character";
                return false;
            }
            result |= PROT_READ;
        } else if (character == 'w') {
            if (index != 1) {
                *error = "'w' must be the second permission character";
                return false;
            }
            result |= PROT_WRITE;
        } else if (character == 'x') {
            if (index != 2) {
                *error = "'x' must be the third permission character";
                return false;
            }
            result |= PROT_EXEC;
        } else if (character != '-') {
            *error = "permissions may only contain 'r', 'w', 'x' or '-'";
            return false;
        }
    }
    *protection = result;
    return true;
}

} // namespace

AgentHelpers::AgentHelpers() = default;

AgentHelpers::~AgentHelpers() {
    shutdown();
}

bool AgentHelpers::initialize(DejavuRuntime *runtime, std::string *error) {
    if (runtime == nullptr) {
        *error = "invalid helper initialization argument";
        return false;
    }
    runtime_ = runtime;
    return true;
}

bool AgentHelpers::register_lua(dejavu_control_session *session, std::string *error) {
    if (runtime_ == nullptr || session == nullptr) {
        *error = "helper manager is not initialized";
        return false;
    }
    struct Registration {
        const char *module;
        const char *name;
        dejavu_host_function function;
    };
    const Registration registrations[] = {
        {"fs", "read", AgentHelpers::lua_fs_read},
        {"fs", "write", AgentHelpers::lua_fs_write},
        {"fs", "exists", AgentHelpers::lua_fs_exists},
        {"fs", "remove", AgentHelpers::lua_fs_remove},
        {"fs", "list", AgentHelpers::lua_fs_list},
        {"fs", "stat", AgentHelpers::lua_fs_stat},
        {"module", "load", AgentHelpers::lua_module_load},
        {"module", "unload", AgentHelpers::lua_module_unload},
        {"module", "loaded", AgentHelpers::lua_module_loaded},
        {"module", "symbols", AgentHelpers::lua_module_symbols},
        {"module", "resolve", AgentHelpers::lua_module_resolve},
        {"mem", "read", AgentHelpers::lua_mem_read},
        {"mem", "write", AgentHelpers::lua_mem_write},
        {"mem", "read_u8", AgentHelpers::lua_mem_read_u8},
        {"mem", "read_u16", AgentHelpers::lua_mem_read_u16},
        {"mem", "read_u32", AgentHelpers::lua_mem_read_u32},
        {"mem", "read_u64", AgentHelpers::lua_mem_read_u64},
        {"mem", "write_u8", AgentHelpers::lua_mem_write_u8},
        {"mem", "write_u16", AgentHelpers::lua_mem_write_u16},
        {"mem", "write_u32", AgentHelpers::lua_mem_write_u32},
        {"mem", "write_u64", AgentHelpers::lua_mem_write_u64},
        {"mem", "dump", AgentHelpers::lua_mem_dump},
        {"mem", "query", AgentHelpers::lua_mem_query},
        {"mem", "protect", AgentHelpers::lua_mem_protect},
        {"mem", "maps", AgentHelpers::lua_mem_maps},
    };
    char runtime_error[512];
    for (const Registration &registration : registrations) {
        const int status = runtime_->control_session_register(
            session, registration.module, registration.name, registration.function, this,
            runtime_error, sizeof(runtime_error));
        if (status != DEJAVU_OK) {
            *error = runtime_error;
            return false;
        }
    }
    return true;
}

void AgentHelpers::shutdown() {
    std::map<long long, Library> libraries;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        libraries.swap(libraries_);
    }
    for (const auto &entry : libraries) {
        if (entry.second.handle != nullptr) {
            dlclose(entry.second.handle);
        }
    }
    runtime_ = nullptr;
}

bool AgentHelpers::library_path(long long id, std::string *path, std::string *error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = libraries_.find(id);
    if (iterator == libraries_.end()) {
        *error = "unknown library id: " + std::to_string(id);
        return false;
    }
    *path = iterator->second.path;
    return true;
}

int AgentHelpers::lua_fs_read(dejavu_control_session *, void *, const dejavu_value *arguments,
                              size_t argument_count, dejavu_value *result, char *error,
                              size_t error_size) {
    if (argument_count != 1 || arguments[0].type != DEJAVU_VALUE_STRING) {
        copy_error(error, error_size, "fs.read expects a path");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    const std::string path = value_string(arguments[0]);
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        copy_error(error, error_size, errno_message("open", path));
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    struct stat status = {};
    if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0) {
        close(fd);
        copy_error(error, error_size, "not a regular file: " + path);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    if (static_cast<unsigned long long>(status.st_size) > kMaximumReadSize) {
        close(fd);
        copy_error(error, error_size, "file is too large to read: " + path);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    std::string content(static_cast<size_t>(status.st_size), '\0');
    size_t offset = 0;
    while (offset < content.size()) {
        const ssize_t count = read(fd, content.data() + offset, content.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            close(fd);
            copy_error(error, error_size, errno_message("read", path));
            return DEJAVU_ERROR_LUA_EXECUTE;
        }
        offset += static_cast<size_t>(count);
    }
    close(fd);
    if (!set_owned_string(result, content)) {
        copy_error(error, error_size, "unable to allocate file content");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    return DEJAVU_OK;
}

int AgentHelpers::lua_fs_write(dejavu_control_session *, void *, const dejavu_value *arguments,
                               size_t argument_count, dejavu_value *result, char *error,
                               size_t error_size) {
    if (argument_count < 2 || argument_count > 3 || arguments[0].type != DEJAVU_VALUE_STRING ||
        arguments[1].type != DEJAVU_VALUE_STRING ||
        (argument_count == 3 && arguments[2].type != DEJAVU_VALUE_BOOLEAN)) {
        copy_error(error, error_size, "fs.write expects path, data and optional append boolean");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    const std::string path = value_string(arguments[0]);
    const std::string content = value_string(arguments[1]);
    const bool append = argument_count == 3 && arguments[2].boolean_value != 0;
    int flags = O_WRONLY | O_CREAT | O_CLOEXEC;
    flags |= append ? O_APPEND : O_TRUNC;
    const int fd = open(path.c_str(), flags, 0644);
    if (fd < 0) {
        copy_error(error, error_size, errno_message("open", path));
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    size_t offset = 0;
    while (offset < content.size()) {
        const ssize_t count = write(fd, content.data() + offset, content.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            close(fd);
            copy_error(error, error_size, errno_message("write", path));
            return DEJAVU_ERROR_LUA_EXECUTE;
        }
        offset += static_cast<size_t>(count);
    }
    close(fd);
    result->type = DEJAVU_VALUE_INTEGER;
    result->integer_value = static_cast<long long>(content.size());
    return DEJAVU_OK;
}

int AgentHelpers::lua_fs_exists(dejavu_control_session *, void *, const dejavu_value *arguments,
                                size_t argument_count, dejavu_value *result, char *error,
                                size_t error_size) {
    if (argument_count != 1 || arguments[0].type != DEJAVU_VALUE_STRING) {
        copy_error(error, error_size, "fs.exists expects a path");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    struct stat status = {};
    const bool exists = stat(value_string(arguments[0]).c_str(), &status) == 0;
    result->type = DEJAVU_VALUE_BOOLEAN;
    result->boolean_value = exists ? 1 : 0;
    return DEJAVU_OK;
}

int AgentHelpers::lua_fs_remove(dejavu_control_session *, void *, const dejavu_value *arguments,
                                size_t argument_count, dejavu_value *result, char *error,
                                size_t error_size) {
    if (argument_count != 1 || arguments[0].type != DEJAVU_VALUE_STRING) {
        copy_error(error, error_size, "fs.remove expects a path");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    const std::string path = value_string(arguments[0]);
    if (unlink(path.c_str()) != 0) {
        copy_error(error, error_size, errno_message("unlink", path));
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    result->type = DEJAVU_VALUE_BOOLEAN;
    result->boolean_value = 1;
    return DEJAVU_OK;
}

int AgentHelpers::lua_fs_list(dejavu_control_session *, void *, const dejavu_value *arguments,
                              size_t argument_count, dejavu_value *result, char *error,
                              size_t error_size) {
    if (argument_count != 1 || arguments[0].type != DEJAVU_VALUE_STRING) {
        copy_error(error, error_size, "fs.list expects a directory path");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    const std::string path = value_string(arguments[0]);
    DIR *directory = opendir(path.c_str());
    if (directory == nullptr) {
        copy_error(error, error_size, errno_message("opendir", path));
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    std::string output;
    while (const dirent *entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        if (!output.empty()) {
            output += "\n";
        }
        output += name;
        if (output.size() > kMaximumListOutput) {
            output.resize(kMaximumListOutput);
            output += "\n# truncated";
            break;
        }
    }
    closedir(directory);
    if (!set_owned_string(result, output)) {
        copy_error(error, error_size, "unable to allocate directory list");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    return DEJAVU_OK;
}

int AgentHelpers::lua_fs_stat(dejavu_control_session *, void *, const dejavu_value *arguments,
                              size_t argument_count, dejavu_value *result, char *error,
                              size_t error_size) {
    if (argument_count != 1 || arguments[0].type != DEJAVU_VALUE_STRING) {
        copy_error(error, error_size, "fs.stat expects a path");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    const std::string path = value_string(arguments[0]);
    struct stat status = {};
    if (stat(path.c_str(), &status) != 0) {
        copy_error(error, error_size, errno_message("stat", path));
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    char text[256];
    snprintf(text, sizeof(text), "size=%llu mode=0%o mtime=%lld type=%s",
             static_cast<unsigned long long>(status.st_size),
             static_cast<unsigned int>(status.st_mode & 07777),
             static_cast<long long>(status.st_mtime),
             S_ISDIR(status.st_mode)   ? "dir"
             : S_ISREG(status.st_mode) ? "file"
                                       : "other");
    if (!set_owned_string(result, text)) {
        copy_error(error, error_size, "unable to allocate stat result");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    return DEJAVU_OK;
}

int AgentHelpers::lua_module_load(dejavu_control_session *, void *context,
                                  const dejavu_value *arguments, size_t argument_count,
                                  dejavu_value *result, char *error, size_t error_size) {
    auto *helpers = static_cast<AgentHelpers *>(context);
    if (helpers == nullptr || argument_count < 1 || argument_count > 2 ||
        arguments[0].type != DEJAVU_VALUE_STRING ||
        (argument_count == 2 && arguments[1].type != DEJAVU_VALUE_STRING)) {
        copy_error(error, error_size, "module.load expects a path and optional scope string");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    const std::string path = value_string(arguments[0]);
    bool global = false;
    if (argument_count == 2) {
        const std::string scope = value_string(arguments[1]);
        if (scope == "global") {
            global = true;
        } else if (scope != "local") {
            copy_error(error, error_size, "module.load scope must be 'local' or 'global'");
            return DEJAVU_ERROR_INVALID_ARGUMENT;
        }
    }
    void *handle = dlopen(path.c_str(), RTLD_NOW | (global ? RTLD_GLOBAL : RTLD_LOCAL));
    if (handle == nullptr) {
        const char *detail = dlerror();
        copy_error(error, error_size,
                   std::string("dlopen failed for ") + path + ": " +
                       (detail != nullptr ? detail : "unknown error"));
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    long long id = 0;
    {
        std::lock_guard<std::mutex> lock(helpers->mutex_);
        id = helpers->next_library_id_++;
        helpers->libraries_[id] = Library{handle, path};
    }
    result->type = DEJAVU_VALUE_INTEGER;
    result->integer_value = id;
    return DEJAVU_OK;
}

int AgentHelpers::lua_module_unload(dejavu_control_session *, void *context,
                                    const dejavu_value *arguments, size_t argument_count,
                                    dejavu_value *result, char *error, size_t error_size) {
    auto *helpers = static_cast<AgentHelpers *>(context);
    if (helpers == nullptr || argument_count != 1 || arguments[0].type != DEJAVU_VALUE_INTEGER ||
        arguments[0].integer_value <= 0) {
        copy_error(error, error_size, "module.unload expects a positive library id");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    void *handle = nullptr;
    {
        std::lock_guard<std::mutex> lock(helpers->mutex_);
        const auto iterator = helpers->libraries_.find(arguments[0].integer_value);
        if (iterator == helpers->libraries_.end()) {
            copy_error(error, error_size,
                       "unknown library id: " + std::to_string(arguments[0].integer_value));
            return DEJAVU_ERROR_LUA_EXECUTE;
        }
        handle = iterator->second.handle;
        helpers->libraries_.erase(iterator);
    }
    if (handle != nullptr) {
        dlclose(handle);
    }
    result->type = DEJAVU_VALUE_BOOLEAN;
    result->boolean_value = 1;
    return DEJAVU_OK;
}

int AgentHelpers::lua_module_loaded(dejavu_control_session *, void *context, const dejavu_value *,
                                    size_t argument_count, dejavu_value *result, char *error,
                                    size_t error_size) {
    auto *helpers = static_cast<AgentHelpers *>(context);
    if (helpers == nullptr || argument_count != 0) {
        copy_error(error, error_size, "module.loaded takes no arguments");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::string output;
    {
        std::lock_guard<std::mutex> lock(helpers->mutex_);
        for (const auto &entry : helpers->libraries_) {
            if (!output.empty()) {
                output += "\n";
            }
            output += std::to_string(entry.first);
            output += "\t";
            output += entry.second.path;
        }
    }
    if (!set_owned_string(result, output)) {
        copy_error(error, error_size, "unable to allocate loaded library list");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    return DEJAVU_OK;
}

int AgentHelpers::lua_module_symbols(dejavu_control_session *, void *context,
                                     const dejavu_value *arguments, size_t argument_count,
                                     dejavu_value *result, char *error, size_t error_size) {
    auto *helpers = static_cast<AgentHelpers *>(context);
    if (helpers == nullptr || argument_count < 1 || argument_count > 2 ||
        (arguments[0].type != DEJAVU_VALUE_STRING && arguments[0].type != DEJAVU_VALUE_INTEGER) ||
        (argument_count == 2 && arguments[1].type != DEJAVU_VALUE_STRING)) {
        copy_error(error, error_size,
                   "module.symbols expects a library id/name and optional name filter");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::string path;
    if (arguments[0].type == DEJAVU_VALUE_INTEGER) {
        if (arguments[0].integer_value <= 0) {
            copy_error(error, error_size, "module.symbols library id must be positive");
            return DEJAVU_ERROR_INVALID_ARGUMENT;
        }
        std::string lookup_error;
        if (!helpers->library_path(arguments[0].integer_value, &path, &lookup_error)) {
            copy_error(error, error_size, lookup_error);
            return DEJAVU_ERROR_LUA_EXECUTE;
        }
    } else {
        path = value_string(arguments[0]);
    }
    const std::string filter = argument_count == 2 ? value_string(arguments[1]) : std::string();
    ArtElfResolver resolver;
    std::string resolver_error;
    if (!resolver.open_loaded(base_name(path), &resolver_error)) {
        copy_error(error, error_size,
                   "unable to inspect loaded library " + path + ": " + resolver_error);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    std::vector<ArtElfSymbol> symbols;
    if (!resolver.enumerate_symbols(&symbols, &resolver_error)) {
        copy_error(error, error_size, resolver_error);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    std::sort(
        symbols.begin(), symbols.end(),
        [](const ArtElfSymbol &left, const ArtElfSymbol &right) { return left.name < right.name; });
    std::string output;
    size_t emitted = 0;
    for (const ArtElfSymbol &symbol : symbols) {
        if (!filter.empty() && symbol.name.find(filter) == std::string::npos) {
            continue;
        }
        if (emitted >= kMaximumSymbols || output.size() >= kMaximumSymbolOutput) {
            output += "\n# truncated";
            break;
        }
        char address_text[32];
        snprintf(address_text, sizeof(address_text), "0x%llx",
                 static_cast<unsigned long long>(symbol.address));
        if (!output.empty()) {
            output += "\n";
        }
        output += symbol.name;
        output += "\t";
        output += symbol_type_letter(symbol.type);
        output += "\t";
        output += symbol_bind_name(symbol.bind);
        output += "\t";
        output += address_text;
        ++emitted;
    }
    if (!set_owned_string(result, output)) {
        copy_error(error, error_size, "unable to allocate symbol list");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    return DEJAVU_OK;
}

int AgentHelpers::lua_module_resolve(dejavu_control_session *, void *context,
                                     const dejavu_value *arguments, size_t argument_count,
                                     dejavu_value *result, char *error, size_t error_size) {
    auto *helpers = static_cast<AgentHelpers *>(context);
    if (helpers == nullptr || argument_count != 2 ||
        (arguments[0].type != DEJAVU_VALUE_STRING && arguments[0].type != DEJAVU_VALUE_INTEGER) ||
        arguments[1].type != DEJAVU_VALUE_STRING) {
        copy_error(error, error_size, "module.resolve expects a library id/name and a symbol");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    const std::string symbol_name = value_string(arguments[1]);
    std::string path;
    void *handle = nullptr;
    if (arguments[0].type == DEJAVU_VALUE_INTEGER) {
        if (arguments[0].integer_value <= 0) {
            copy_error(error, error_size, "module.resolve library id must be positive");
            return DEJAVU_ERROR_INVALID_ARGUMENT;
        }
        {
            std::lock_guard<std::mutex> lock(helpers->mutex_);
            const auto iterator = helpers->libraries_.find(arguments[0].integer_value);
            if (iterator == helpers->libraries_.end()) {
                copy_error(error, error_size,
                           "unknown library id: " + std::to_string(arguments[0].integer_value));
                return DEJAVU_ERROR_LUA_EXECUTE;
            }
            path = iterator->second.path;
            handle = iterator->second.handle;
        }
    } else {
        path = value_string(arguments[0]);
        handle = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
    }
    void *address = nullptr;
    if (handle != nullptr) {
        dlerror();
        address = dlsym(handle, symbol_name.c_str());
        const char *detail = dlerror();
        if (detail != nullptr) {
            address = nullptr;
        }
        if (arguments[0].type == DEJAVU_VALUE_STRING) {
            dlclose(handle);
        }
    }
    if (address == nullptr) {
        ArtElfResolver resolver;
        std::string resolver_error;
        if (resolver.open_loaded(base_name(path), &resolver_error)) {
            address = resolver.find(symbol_name);
        }
    }
    if (address == nullptr) {
        result->type = DEJAVU_VALUE_NIL;
        return DEJAVU_OK;
    }
    result->type = DEJAVU_VALUE_INTEGER;
    result->integer_value = static_cast<long long>(reinterpret_cast<uintptr_t>(address));
    return DEJAVU_OK;
}

namespace {

int read_integer(const dejavu_value *arguments, size_t argument_count, size_t width,
                 dejavu_value *result, char *error, size_t error_size) {
    if (argument_count != 1 || arguments[0].type != DEJAVU_VALUE_INTEGER) {
        copy_error(error, error_size, "mem.read_u* expects an address");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    uintptr_t address = 0;
    std::string message;
    if (!address_from_value(arguments[0], &address, &message)) {
        copy_error(error, error_size, message);
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    if (!mapped_range(address, width, 'r', nullptr, &message)) {
        copy_error(error, error_size, message);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    const uint64_t value = load_unsigned(reinterpret_cast<const void *>(address), width);
    result->type = DEJAVU_VALUE_INTEGER;
    result->integer_value = static_cast<long long>(value);
    return DEJAVU_OK;
}

int write_integer(const dejavu_value *arguments, size_t argument_count, size_t width,
                  dejavu_value *result, char *error, size_t error_size) {
    if (argument_count != 2 || arguments[0].type != DEJAVU_VALUE_INTEGER ||
        arguments[1].type != DEJAVU_VALUE_INTEGER) {
        copy_error(error, error_size, "mem.write_u* expects an address and an integer");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    uintptr_t address = 0;
    std::string message;
    if (!address_from_value(arguments[0], &address, &message)) {
        copy_error(error, error_size, message);
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    if (!mapped_range(address, width, 'w', nullptr, &message)) {
        copy_error(error, error_size, message);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    store_unsigned(reinterpret_cast<void *>(address), width,
                   static_cast<uint64_t>(arguments[1].integer_value));
    result->type = DEJAVU_VALUE_BOOLEAN;
    result->boolean_value = 1;
    return DEJAVU_OK;
}

} // namespace

int AgentHelpers::lua_mem_read(dejavu_control_session *, void *, const dejavu_value *arguments,
                               size_t argument_count, dejavu_value *result, char *error,
                               size_t error_size) {
    if (argument_count != 2 || arguments[0].type != DEJAVU_VALUE_INTEGER ||
        arguments[1].type != DEJAVU_VALUE_INTEGER) {
        copy_error(error, error_size, "mem.read expects an address and a size");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    if (arguments[1].integer_value <= 0 ||
        static_cast<unsigned long long>(arguments[1].integer_value) > kMaximumReadSize) {
        copy_error(error, error_size, "mem.read size must be between 1 and 64 MiB");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    uintptr_t address = 0;
    std::string message;
    if (!address_from_value(arguments[0], &address, &message)) {
        copy_error(error, error_size, message);
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    const size_t size = static_cast<size_t>(arguments[1].integer_value);
    if (!mapped_range(address, size, 'r', nullptr, &message)) {
        copy_error(error, error_size, message);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    std::string content(size, '\0');
    memcpy(content.data(), reinterpret_cast<const void *>(address), size);
    if (!set_owned_string(result, content)) {
        copy_error(error, error_size, "unable to allocate memory content");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    return DEJAVU_OK;
}

int AgentHelpers::lua_mem_write(dejavu_control_session *, void *, const dejavu_value *arguments,
                                size_t argument_count, dejavu_value *result, char *error,
                                size_t error_size) {
    if (argument_count != 2 || arguments[0].type != DEJAVU_VALUE_INTEGER ||
        arguments[1].type != DEJAVU_VALUE_STRING) {
        copy_error(error, error_size, "mem.write expects an address and a string");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    uintptr_t address = 0;
    std::string message;
    if (!address_from_value(arguments[0], &address, &message)) {
        copy_error(error, error_size, message);
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    const std::string content = value_string(arguments[1]);
    if (content.empty()) {
        result->type = DEJAVU_VALUE_INTEGER;
        result->integer_value = 0;
        return DEJAVU_OK;
    }
    if (content.size() > kMaximumReadSize) {
        copy_error(error, error_size, "mem.write data must not exceed 64 MiB");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    if (!mapped_range(address, content.size(), 'w', nullptr, &message)) {
        copy_error(error, error_size, message);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    memcpy(reinterpret_cast<void *>(address), content.data(), content.size());
    result->type = DEJAVU_VALUE_INTEGER;
    result->integer_value = static_cast<long long>(content.size());
    return DEJAVU_OK;
}

int AgentHelpers::lua_mem_read_u8(dejavu_control_session *, void *, const dejavu_value *arguments,
                                  size_t argument_count, dejavu_value *result, char *error,
                                  size_t error_size) {
    return read_integer(arguments, argument_count, 1, result, error, error_size);
}

int AgentHelpers::lua_mem_read_u16(dejavu_control_session *, void *, const dejavu_value *arguments,
                                   size_t argument_count, dejavu_value *result, char *error,
                                   size_t error_size) {
    return read_integer(arguments, argument_count, 2, result, error, error_size);
}

int AgentHelpers::lua_mem_read_u32(dejavu_control_session *, void *, const dejavu_value *arguments,
                                   size_t argument_count, dejavu_value *result, char *error,
                                   size_t error_size) {
    return read_integer(arguments, argument_count, 4, result, error, error_size);
}

int AgentHelpers::lua_mem_read_u64(dejavu_control_session *, void *, const dejavu_value *arguments,
                                   size_t argument_count, dejavu_value *result, char *error,
                                   size_t error_size) {
    return read_integer(arguments, argument_count, 8, result, error, error_size);
}

int AgentHelpers::lua_mem_write_u8(dejavu_control_session *, void *, const dejavu_value *arguments,
                                   size_t argument_count, dejavu_value *result, char *error,
                                   size_t error_size) {
    return write_integer(arguments, argument_count, 1, result, error, error_size);
}

int AgentHelpers::lua_mem_write_u16(dejavu_control_session *, void *, const dejavu_value *arguments,
                                    size_t argument_count, dejavu_value *result, char *error,
                                    size_t error_size) {
    return write_integer(arguments, argument_count, 2, result, error, error_size);
}

int AgentHelpers::lua_mem_write_u32(dejavu_control_session *, void *, const dejavu_value *arguments,
                                    size_t argument_count, dejavu_value *result, char *error,
                                    size_t error_size) {
    return write_integer(arguments, argument_count, 4, result, error, error_size);
}

int AgentHelpers::lua_mem_write_u64(dejavu_control_session *, void *, const dejavu_value *arguments,
                                    size_t argument_count, dejavu_value *result, char *error,
                                    size_t error_size) {
    return write_integer(arguments, argument_count, 8, result, error, error_size);
}

int AgentHelpers::lua_mem_query(dejavu_control_session *, void *, const dejavu_value *arguments,
                                size_t argument_count, dejavu_value *result, char *error,
                                size_t error_size) {
    if (argument_count != 1 || arguments[0].type != DEJAVU_VALUE_INTEGER) {
        copy_error(error, error_size, "mem.query expects an address");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    uintptr_t address = 0;
    std::string message;
    if (!address_from_value(arguments[0], &address, &message)) {
        copy_error(error, error_size, message);
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::vector<MemoryMapping> mappings;
    if (!read_mappings(&mappings, &message)) {
        copy_error(error, error_size, message);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    for (const MemoryMapping &mapping : mappings) {
        if (address < mapping.start || address >= mapping.end) {
            continue;
        }
        char line[512];
        snprintf(line, sizeof(line), "0x%llx-0x%llx %s %s",
                 static_cast<unsigned long long>(mapping.start),
                 static_cast<unsigned long long>(mapping.end), mapping.permissions.c_str(),
                 mapping.path.c_str());
        if (!set_owned_string(result, line)) {
            copy_error(error, error_size, "unable to allocate mapping description");
            return DEJAVU_ERROR_OUT_OF_MEMORY;
        }
        return DEJAVU_OK;
    }
    result->type = DEJAVU_VALUE_NIL;
    return DEJAVU_OK;
}

int AgentHelpers::lua_mem_protect(dejavu_control_session *, void *, const dejavu_value *arguments,
                                  size_t argument_count, dejavu_value *result, char *error,
                                  size_t error_size) {
    if (argument_count != 3 || arguments[0].type != DEJAVU_VALUE_INTEGER ||
        arguments[1].type != DEJAVU_VALUE_INTEGER || arguments[2].type != DEJAVU_VALUE_STRING) {
        copy_error(error, error_size, "mem.protect expects address, size and permissions");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    if (arguments[1].integer_value <= 0) {
        copy_error(error, error_size, "mem.protect size must be positive");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    uintptr_t address = 0;
    std::string message;
    if (!address_from_value(arguments[0], &address, &message)) {
        copy_error(error, error_size, message);
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    int protection = 0;
    if (!parse_protection(value_string(arguments[2]), &protection, &message)) {
        copy_error(error, error_size, message);
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        copy_error(error, error_size, "unable to determine the page size");
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    const uintptr_t page = static_cast<uintptr_t>(page_size);
    const uintptr_t start = address - address % page;
    const uintptr_t end = address + static_cast<uintptr_t>(arguments[1].integer_value);
    if (end < address) {
        copy_error(error, error_size, "address range overflows");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    const uintptr_t rounded_end = end % page == 0 ? end : end + (page - end % page);
    if (rounded_end < end) {
        copy_error(error, error_size, "address range overflows");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::vector<MemoryMapping> mappings;
    if (!read_mappings(&mappings, &message)) {
        copy_error(error, error_size, message);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    std::string previous = "---";
    for (const MemoryMapping &mapping : mappings) {
        if (start >= mapping.start && start < mapping.end) {
            previous = mapping.permissions.substr(0, 3);
            break;
        }
    }
    if (mprotect(reinterpret_cast<void *>(start), static_cast<size_t>(rounded_end - start),
                 protection) != 0) {
        copy_error(error, error_size, std::string("mprotect failed: ") + strerror(errno));
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    if (!set_owned_string(result, previous)) {
        copy_error(error, error_size, "unable to allocate previous permissions");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    return DEJAVU_OK;
}

int AgentHelpers::lua_mem_maps(dejavu_control_session *, void *, const dejavu_value *arguments,
                               size_t argument_count, dejavu_value *result, char *error,
                               size_t error_size) {
    if (argument_count > 1 || (argument_count == 1 && arguments[0].type != DEJAVU_VALUE_STRING)) {
        copy_error(error, error_size, "mem.maps expects an optional path/perms filter");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    const std::string filter = argument_count == 1 ? value_string(arguments[0]) : std::string();
    std::ifstream maps("/proc/self/maps");
    if (!maps.is_open()) {
        copy_error(error, error_size,
                   std::string("unable to open /proc/self/maps: ") + strerror(errno));
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    std::string output;
    std::string line;
    while (std::getline(maps, line)) {
        if (!filter.empty() && line.find(filter) == std::string::npos) {
            continue;
        }
        if (!output.empty()) {
            output += "\n";
        }
        output += line;
        if (output.size() > kMaximumListOutput) {
            output.resize(kMaximumListOutput);
            output += "\n# truncated";
            break;
        }
    }
    if (!set_owned_string(result, output)) {
        copy_error(error, error_size, "unable to allocate mapping list");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    return DEJAVU_OK;
}

int AgentHelpers::lua_mem_dump(dejavu_control_session *, void *, const dejavu_value *arguments,
                               size_t argument_count, dejavu_value *result, char *error,
                               size_t error_size) {
    if (argument_count < 2 || argument_count > 3 || arguments[0].type != DEJAVU_VALUE_INTEGER ||
        arguments[1].type != DEJAVU_VALUE_INTEGER ||
        (argument_count == 3 && arguments[2].type != DEJAVU_VALUE_INTEGER)) {
        copy_error(error, error_size, "mem.dump expects address, size and optional line width");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    if (arguments[1].integer_value <= 0 ||
        static_cast<unsigned long long>(arguments[1].integer_value) > kMaximumReadSize) {
        copy_error(error, error_size, "mem.dump size must be between 1 and 64 MiB");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    size_t width = 16;
    if (argument_count == 3) {
        if (arguments[2].integer_value < 1 || arguments[2].integer_value > 64) {
            copy_error(error, error_size, "mem.dump line width must be between 1 and 64");
            return DEJAVU_ERROR_INVALID_ARGUMENT;
        }
        width = static_cast<size_t>(arguments[2].integer_value);
    }
    uintptr_t address = 0;
    std::string message;
    if (!address_from_value(arguments[0], &address, &message)) {
        copy_error(error, error_size, message);
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    const size_t size = static_cast<size_t>(arguments[1].integer_value);
    if (!mapped_range(address, size, 'r', nullptr, &message)) {
        copy_error(error, error_size, message);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    const auto *bytes = reinterpret_cast<const unsigned char *>(address);
    std::string output;
    for (size_t offset = 0; offset < size; ++offset) {
        if (offset % width == 0) {
            if (offset != 0) {
                output += "\n";
            }
            char prefix[32];
            snprintf(prefix, sizeof(prefix), "0x%016llx  ",
                     static_cast<unsigned long long>(address + offset));
            output += prefix;
        }
        char byte_text[8];
        snprintf(byte_text, sizeof(byte_text), "%02x ", bytes[offset]);
        output += byte_text;
        const bool line_end = (offset + 1) % width == 0 || offset + 1 == size;
        if (line_end) {
            const size_t line_start = offset - (offset % width);
            const size_t remaining = width - ((offset - line_start) + 1);
            for (size_t pad = 0; pad < remaining; ++pad) {
                output += "   ";
            }
            output += " |";
            for (size_t index = line_start; index <= offset; ++index) {
                const unsigned char byte = bytes[index];
                output += (byte >= 0x20 && byte <= 0x7e) ? static_cast<char>(byte) : '.';
            }
            output += "|";
        }
        if (output.size() > kMaximumListOutput) {
            output.resize(kMaximumListOutput);
            output += "\n# truncated";
            break;
        }
    }
    if (!set_owned_string(result, output)) {
        copy_error(error, error_size, "unable to allocate memory dump");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    return DEJAVU_OK;
}
