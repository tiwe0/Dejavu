#include "agent_helpers.h"

#include <dirent.h>
#include <elf.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
