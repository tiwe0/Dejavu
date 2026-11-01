#include "dejavu_plugin.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {

constexpr size_t kCapturePayloadLimit = 900;

struct CaptureState {
    const DejavuPluginHost *host = nullptr;
    const DejavuModuleInterface *module = nullptr;
    const DejavuHookInterface *hook = nullptr;
    std::mutex mutex;
    int fd = -1;
    std::string path;
    void *write_target = nullptr;
    void *read_target = nullptr;
    int (*original_write)(void *, const void *, int) = nullptr;
    int (*original_read)(void *, void *, int) = nullptr;
    unsigned long long write_calls = 0;
    unsigned long long read_calls = 0;
    unsigned long long write_bytes = 0;
    unsigned long long read_bytes = 0;
};

CaptureState *g_active = nullptr;

char *make_string(CaptureState *state, const std::string &text) {
    if (state == nullptr || state->host == nullptr || state->host->alloc == nullptr) {
        return nullptr;
    }
    char *copy = static_cast<char *>(state->host->alloc(text.size() + 1));
    if (copy == nullptr) {
        return nullptr;
    }
    std::memcpy(copy, text.data(), text.size());
    copy[text.size()] = '\0';
    return copy;
}

void set_result_string(CaptureState *state, dejavu_value *result, const std::string &text) {
    char *copy = make_string(state, text);
    if (copy == nullptr) {
        result->type = DEJAVU_VALUE_NIL;
        return;
    }
    result->type = DEJAVU_VALUE_STRING;
    result->owned = 1;
    result->string_value = copy;
    result->string_size = text.size();
}

void write_all(int fd, const char *data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        const ssize_t written = write(fd, data + offset, size - offset);
        if (written <= 0) {
            return;
        }
        offset += static_cast<size_t>(written);
    }
}

void capture_record(CaptureState *state, char direction, const void *buffer, int length) {
    if (state == nullptr || buffer == nullptr || length <= 0) {
        return;
    }
    char line[1200];
    const int header =
        std::snprintf(line, sizeof(line), "%s len=%d ", direction == 'O' ? "OUT" : "IN", length);
    if (header <= 0) {
        return;
    }
    size_t offset = static_cast<size_t>(header);
    const size_t payload = static_cast<size_t>(length) < kCapturePayloadLimit
                               ? static_cast<size_t>(length)
                               : kCapturePayloadLimit;
    const auto *bytes = static_cast<const unsigned char *>(buffer);
    for (size_t index = 0; index < payload && offset + 1 < sizeof(line); ++index) {
        const unsigned char value = bytes[index];
        line[offset++] = (value >= 32 && value < 127) ? static_cast<char>(value) : '.';
    }
    line[offset++] = '\n';
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->fd < 0) {
        return;
    }
    write_all(state->fd, line, offset);
    if (direction == 'O') {
        ++state->write_calls;
        state->write_bytes += static_cast<unsigned long long>(length);
    } else {
        ++state->read_calls;
        state->read_bytes += static_cast<unsigned long long>(length);
    }
}

int hook_write(void *ssl, const void *buffer, int length) {
    CaptureState *state = g_active;
    if (state == nullptr || state->original_write == nullptr) {
        return -1;
    }
    capture_record(state, 'O', buffer, length);
    return state->original_write(ssl, buffer, length);
}

int hook_read(void *ssl, void *buffer, int length) {
    CaptureState *state = g_active;
    if (state == nullptr || state->original_read == nullptr) {
        return -1;
    }
    const int result = state->original_read(ssl, buffer, length);
    if (result > 0) {
        capture_record(state, 'I', buffer, result);
    }
    return result;
}

bool stop_capture(CaptureState *state, std::string *error) {
    if (state == nullptr) {
        *error = "capture state is missing";
        return false;
    }
    bool failed = false;
    if (state->write_target != nullptr && state->hook != nullptr) {
        char hook_error[256] = {};
        if (state->hook->uninstall(state->host->host_context, state->write_target, hook_error,
                                   sizeof(hook_error)) != 0) {
            *error = hook_error[0] != '\0' ? hook_error : "unable to uninstall SSL_write hook";
            failed = true;
        } else {
            state->write_target = nullptr;
            state->original_write = nullptr;
        }
    }
    if (state->read_target != nullptr && state->hook != nullptr) {
        char hook_error[256] = {};
        if (state->hook->uninstall(state->host->host_context, state->read_target, hook_error,
                                   sizeof(hook_error)) != 0) {
            *error = hook_error[0] != '\0' ? hook_error : "unable to uninstall SSL_read hook";
            failed = true;
        } else {
            state->read_target = nullptr;
            state->original_read = nullptr;
        }
    }
    if (failed) {
        return false;
    }
    g_active = nullptr;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->fd >= 0) {
            close(state->fd);
            state->fd = -1;
        }
        state->path.clear();
    }
    return true;
}

std::string status_text(CaptureState *state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->fd < 0) {
        return "capture idle";
    }
    return "capture " + state->path + " out=" + std::to_string(state->write_calls) + "/" +
           std::to_string(state->write_bytes) + "B in=" + std::to_string(state->read_calls) + "/" +
           std::to_string(state->read_bytes) + "B";
}

int lua_start(dejavu_control_session *, void *context, const dejavu_value *arguments,
              size_t argument_count, dejavu_value *result, char *error, size_t error_size) {
    auto *state = static_cast<CaptureState *>(context);
    if (state == nullptr || argument_count != 1 || arguments[0].type != DEJAVU_VALUE_STRING ||
        arguments[0].string_value == nullptr || arguments[0].string_size == 0) {
        std::snprintf(error, error_size, "capture.start expects a device output path");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    const std::string path(arguments[0].string_value, arguments[0].string_size);
    if (state->fd >= 0) {
        std::snprintf(error, error_size, "capture is already active");
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    if (state->module == nullptr || state->hook == nullptr) {
        std::snprintf(error, error_size, "host module/hook interface is unavailable");
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    void *write_target =
        state->module->resolve(state->host->host_context, "libssl.so", "SSL_write");
    void *read_target = state->module->resolve(state->host->host_context, "libssl.so", "SSL_read");
    if (write_target == nullptr || read_target == nullptr) {
        std::snprintf(error, error_size, "libssl.so SSL_write/SSL_read not found");
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        std::snprintf(error, error_size, "unable to open capture file: %s", path.c_str());
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    char hook_error[256] = {};
    void *original_write = nullptr;
    if (state->hook->install(state->host->host_context, write_target,
                             reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(&hook_write)),
                             &original_write, hook_error, sizeof(hook_error)) != 0) {
        close(fd);
        std::snprintf(error, error_size, "%s",
                      hook_error[0] != '\0' ? hook_error : "unable to hook SSL_write");
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    void *original_read = nullptr;
    if (state->hook->install(state->host->host_context, read_target,
                             reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(&hook_read)),
                             &original_read, hook_error, sizeof(hook_error)) != 0) {
        state->hook->uninstall(state->host->host_context, write_target, hook_error,
                               sizeof(hook_error));
        close(fd);
        std::snprintf(error, error_size, "%s",
                      hook_error[0] != '\0' ? hook_error : "unable to hook SSL_read");
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    state->write_target = write_target;
    state->read_target = read_target;
    state->original_write = reinterpret_cast<int (*)(void *, const void *, int)>(original_write);
    state->original_read = reinterpret_cast<int (*)(void *, void *, int)>(original_read);
    state->fd = fd;
    state->path = path;
    g_active = state;
    set_result_string(state, result, "capture started: " + path);
    return DEJAVU_OK;
}

int lua_stop(dejavu_control_session *, void *context, const dejavu_value *, size_t,
             dejavu_value *result, char *error, size_t error_size) {
    auto *state = static_cast<CaptureState *>(context);
    std::string stop_error;
    if (!stop_capture(state, &stop_error)) {
        std::snprintf(error, error_size, "%s", stop_error.c_str());
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    set_result_string(state, result, "capture stopped");
    return DEJAVU_OK;
}

int lua_status(dejavu_control_session *, void *context, const dejavu_value *, size_t,
               dejavu_value *result, char *, size_t) {
    auto *state = static_cast<CaptureState *>(context);
    set_result_string(state, result, status_text(state));
    return DEJAVU_OK;
}

int plugin_initialize(const DejavuPluginHost *host, void **plugin_state, char *error,
                      size_t error_size) {
    if (host == nullptr || plugin_state == nullptr ||
        host->abi_version != DEJAVU_PLUGIN_ABI_VERSION ||
        host->struct_size <
            offsetof(DejavuPluginHost, get_interface) + sizeof(host->get_interface)) {
        std::snprintf(error, error_size, "incompatible plugin host ABI");
        return -1;
    }
    auto *state = new CaptureState();
    state->host = host;
    if (host->get_interface != nullptr) {
        state->module = static_cast<const DejavuModuleInterface *>(
            host->get_interface(host->host_context, "module", DEJAVU_MODULE_INTERFACE_VERSION));
        state->hook = static_cast<const DejavuHookInterface *>(
            host->get_interface(host->host_context, "hook", DEJAVU_HOOK_INTERFACE_VERSION));
    }
    if (state->module == nullptr || state->hook == nullptr) {
        std::snprintf(error, error_size, "capture requires module and hook host interfaces");
        delete state;
        return -1;
    }
    const struct {
        const char *name;
        dejavu_host_function function;
    } registrations[] = {
        {"start", lua_start},
        {"stop", lua_stop},
        {"status", lua_status},
    };
    for (const auto &registration : registrations) {
        if (host->register_function(host->host_context, "capture", registration.name,
                                    registration.function, state, error, error_size) != DEJAVU_OK) {
            delete state;
            return -1;
        }
    }
    *plugin_state = state;
    return 0;
}

void plugin_unload_prepare(void *plugin_state) {
    auto *state = static_cast<CaptureState *>(plugin_state);
    if (state == nullptr) {
        return;
    }
    std::string error;
    if (!stop_capture(state, &error) && state->host != nullptr &&
        state->host->log_message != nullptr) {
        state->host->log_message(state->host->host_context, 5,
                                 ("capture stop failed: " + error).c_str());
    }
}

void plugin_shutdown(void *plugin_state) {
    auto *state = static_cast<CaptureState *>(plugin_state);
    if (state == nullptr) {
        return;
    }
    std::string error;
    stop_capture(state, &error);
    delete state;
}

} // namespace

extern "C" __attribute__((visibility("default"))) const DejavuPluginDescriptor *
dejavu_plugin_entry(void) {
    static DejavuPluginDescriptor descriptor = {
        DEJAVU_PLUGIN_ABI_VERSION,
        sizeof(DejavuPluginDescriptor),
        "capture",
        "1.0.0",
        DEJAVU_PLUGIN_FLAG_UNLOAD_SAFE,
        plugin_initialize,
        nullptr,
        plugin_shutdown,
        DEJAVU_PLUGIN_CAP_MODULE | DEJAVU_PLUGIN_CAP_HOOK,
        plugin_unload_prepare,
    };
    return &descriptor;
}
