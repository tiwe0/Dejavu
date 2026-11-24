#include "dejavu_plugin.h"
#include "rpc_helper.h"

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace {

constexpr size_t kCapturePayloadLimit = 900;
constexpr size_t kMaxRecentRecords = 128;
constexpr size_t kDefaultRpcRecentLimit = 32;
constexpr size_t kMaxRpcRecentLimit = 64;

struct CaptureRecord {
    unsigned long long seq = 0;
    char direction = 'O';
    int length = 0;
    std::string preview;
};

struct CaptureState {
    const DejavuPluginHost *host = nullptr;
    const DejavuModuleInterface *module = nullptr;
    const DejavuHookInterface *hook = nullptr;
    const DejavuStreamInterface *stream = nullptr;
    const DejavuRpcInterface *rpc = nullptr;
    uint64_t rpc_service = 0;
    std::string process_name;
    std::string rpc_route;
    std::mutex mutex;
    bool active = false;
    int fd = -1;
    int stream_handle = 0;
    std::string path;
    void *write_target = nullptr;
    void *read_target = nullptr;
    int (*original_write)(void *, const void *, int) = nullptr;
    int (*original_read)(void *, void *, int) = nullptr;
    unsigned long long write_calls = 0;
    unsigned long long read_calls = 0;
    unsigned long long write_bytes = 0;
    unsigned long long read_bytes = 0;
    unsigned long long record_seq = 0;
    unsigned long long dropped_records = 0;
    std::deque<CaptureRecord> recent;
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
    const size_t payload = static_cast<size_t>(length) < kCapturePayloadLimit
                               ? static_cast<size_t>(length)
                               : kCapturePayloadLimit;
    std::string preview;
    preview.resize(payload);
    const auto *bytes = static_cast<const unsigned char *>(buffer);
    for (size_t index = 0; index < payload; ++index) {
        const unsigned char value = bytes[index];
        preview[index] = (value >= 32 && value < 127) ? static_cast<char>(value) : '.';
    }
    char header[64];
    const int header_len =
        std::snprintf(header, sizeof(header), "%s len=%d ", direction == 'O' ? "OUT" : "IN", length);
    if (header_len <= 0) {
        return;
    }
    std::string line;
    line.reserve(static_cast<size_t>(header_len) + preview.size() + 1);
    line.append(header, static_cast<size_t>(header_len));
    line.append(preview);
    line.push_back('\n');

    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->active) {
        return;
    }
    if (state->fd >= 0) {
        write_all(state->fd, line.data(), line.size());
    }
    if (state->stream_handle > 0 && state->stream != nullptr) {
        state->stream->write(state->host->host_context, state->stream_handle, line.data(),
                             line.size());
    }
    if (state->recent.size() >= kMaxRecentRecords) {
        state->recent.pop_front();
        ++state->dropped_records;
    }
    state->recent.push_back(CaptureRecord{++state->record_seq, direction, length, std::move(preview)});
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

bool start_capture(CaptureState *state, const std::string &path, uint32_t transport,
                   const std::string &endpoint, std::string *error, std::string *summary) {
    if (state == nullptr) {
        *error = "capture state is missing";
        return false;
    }
    if (transport != 0) {
        if (transport < DEJAVU_STREAM_TRANSPORT_TCP || transport > DEJAVU_STREAM_TRANSPORT_KCP) {
            *error = "transport must be 1 (tcp), 2 (udp) or 3 (kcp)";
            return false;
        }
        if (endpoint.empty()) {
            *error = "stream endpoint is required";
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->active || state->fd >= 0 || state->stream_handle > 0) {
            *error = "capture is already active";
            return false;
        }
    }
    if (state->module == nullptr || state->hook == nullptr) {
        *error = "host module/hook interface is unavailable";
        return false;
    }
    void *write_target =
        state->module->resolve(state->host->host_context, "libssl.so", "SSL_write");
    void *read_target = state->module->resolve(state->host->host_context, "libssl.so", "SSL_read");
    if (write_target == nullptr || read_target == nullptr) {
        *error = "libssl.so SSL_write/SSL_read not found";
        return false;
    }

    int stream_handle = 0;
    if (transport != 0) {
        if (state->stream == nullptr) {
            *error = "host stream interface is unavailable";
            return false;
        }
        char stream_error[256] = {};
        stream_handle = state->stream->open(state->host->host_context, transport, endpoint.c_str(),
                                            stream_error, sizeof(stream_error));
        if (stream_handle <= 0) {
            *error = stream_error[0] != '\0' ? stream_error : "unable to open capture stream";
            return false;
        }
    }

    int fd = -1;
    if (!path.empty()) {
        fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (fd < 0) {
            if (stream_handle > 0) {
                state->stream->close(state->host->host_context, stream_handle);
            }
            *error = "unable to open capture file: " + path;
            return false;
        }
    }

    char hook_error[256] = {};
    void *original_write = nullptr;
    if (state->hook->install(state->host->host_context, write_target,
                             reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(&hook_write)),
                             &original_write, hook_error, sizeof(hook_error)) != 0) {
        if (fd >= 0) {
            close(fd);
        }
        if (stream_handle > 0) {
            state->stream->close(state->host->host_context, stream_handle);
        }
        *error = hook_error[0] != '\0' ? hook_error : "unable to hook SSL_write";
        return false;
    }
    void *original_read = nullptr;
    if (state->hook->install(state->host->host_context, read_target,
                             reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(&hook_read)),
                             &original_read, hook_error, sizeof(hook_error)) != 0) {
        state->hook->uninstall(state->host->host_context, write_target, hook_error,
                               sizeof(hook_error));
        if (fd >= 0) {
            close(fd);
        }
        if (stream_handle > 0) {
            state->stream->close(state->host->host_context, stream_handle);
        }
        *error = hook_error[0] != '\0' ? hook_error : "unable to hook SSL_read";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->write_target = write_target;
        state->read_target = read_target;
        state->original_write = reinterpret_cast<int (*)(void *, const void *, int)>(original_write);
        state->original_read = reinterpret_cast<int (*)(void *, void *, int)>(original_read);
        state->fd = fd;
        state->stream_handle = stream_handle;
        state->path = path;
        state->active = true;
    }
    g_active = state;
    if (summary != nullptr) {
        std::string started = "capture started";
        if (!path.empty()) {
            started += ": " + path;
        } else if (stream_handle <= 0) {
            started += ": memory";
        }
        if (stream_handle > 0) {
            started += " stream=" + std::to_string(stream_handle);
        }
        *summary = std::move(started);
    }
    return true;
}

bool stop_capture(CaptureState *state, std::string *error) {
    if (state == nullptr) {
        *error = "capture state is missing";
        return false;
    }
    void *write_target = nullptr;
    void *read_target = nullptr;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        write_target = state->write_target;
        read_target = state->read_target;
    }
    bool failed = false;
    if (write_target != nullptr && state->hook != nullptr) {
        char hook_error[256] = {};
        if (state->hook->uninstall(state->host->host_context, write_target, hook_error,
                                   sizeof(hook_error)) != 0) {
            *error = hook_error[0] != '\0' ? hook_error : "unable to uninstall SSL_write hook";
            failed = true;
        } else {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->write_target = nullptr;
            state->original_write = nullptr;
        }
    }
    if (read_target != nullptr && state->hook != nullptr) {
        char hook_error[256] = {};
        if (state->hook->uninstall(state->host->host_context, read_target, hook_error,
                                   sizeof(hook_error)) != 0) {
            *error = hook_error[0] != '\0' ? hook_error : "unable to uninstall SSL_read hook";
            failed = true;
        } else {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->read_target = nullptr;
            state->original_read = nullptr;
        }
    }
    if (failed) {
        return false;
    }
    if (g_active == state) {
        g_active = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->active = false;
        if (state->fd >= 0) {
            close(state->fd);
            state->fd = -1;
        }
        if (state->stream_handle > 0 && state->stream != nullptr) {
            state->stream->close(state->host->host_context, state->stream_handle);
            state->stream_handle = 0;
        }
        state->path.clear();
    }
    return true;
}

std::string status_text(CaptureState *state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->active) {
        return "capture idle";
    }
    std::string text = "capture ";
    if (!state->path.empty()) {
        text += state->path;
    } else if (state->stream_handle > 0) {
        text += "stream";
    } else {
        text += "memory";
    }
    if (state->stream_handle > 0) {
        text += " stream=" + std::to_string(state->stream_handle);
    }
    text += " out=" + std::to_string(state->write_calls) + "/" +
            std::to_string(state->write_bytes) + "B in=" + std::to_string(state->read_calls) + "/" +
            std::to_string(state->read_bytes) + "B buffered=" + std::to_string(state->recent.size());
    return text;
}

std::string status_json(CaptureState *state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    std::string json = "{\"service\":\"capture\",\"version\":\"1.1.0\",\"active\":";
    json += state->active ? "true" : "false";
    json += ",\"route\":\"" + dejavu_example::json_escape(state->rpc_route) + "\"";
    json += ",\"process\":\"" + dejavu_example::json_escape(state->process_name) + "\"";
    json += ",\"pid\":" + std::to_string(static_cast<unsigned long>(getpid()));
    json += ",\"path\":\"" + dejavu_example::json_escape(state->path) + "\"";
    json += ",\"stream_handle\":" + std::to_string(state->stream_handle);
    json += ",\"write_calls\":" + std::to_string(state->write_calls);
    json += ",\"write_bytes\":" + std::to_string(state->write_bytes);
    json += ",\"read_calls\":" + std::to_string(state->read_calls);
    json += ",\"read_bytes\":" + std::to_string(state->read_bytes);
    json += ",\"buffered_records\":" + std::to_string(state->recent.size());
    json += ",\"dropped_records\":" + std::to_string(state->dropped_records);
    json += "}";
    return json;
}

bool run_probe(CaptureState *state, uint32_t transport, const std::string &endpoint,
               std::string *error) {
    if (state == nullptr || state->stream == nullptr) {
        *error = "host stream interface is unavailable";
        return false;
    }
    if (transport < DEJAVU_STREAM_TRANSPORT_TCP || transport > DEJAVU_STREAM_TRANSPORT_KCP) {
        *error = "invalid stream transport";
        return false;
    }
    if (endpoint.empty()) {
        *error = "stream endpoint is required";
        return false;
    }
    char stream_error[256] = {};
    const int handle = state->stream->open(state->host->host_context, transport, endpoint.c_str(),
                                           stream_error, sizeof(stream_error));
    if (handle <= 0) {
        *error = stream_error[0] != '\0' ? stream_error : "unable to open stream";
        return false;
    }
    const char message[] = "dejavu-stream-probe\n";
    const int status =
        state->stream->write(state->host->host_context, handle, message, sizeof(message) - 1);
    if (transport == DEJAVU_STREAM_TRANSPORT_KCP) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    state->stream->close(state->host->host_context, handle);
    if (status != 0) {
        *error = "stream probe write failed";
        return false;
    }
    return true;
}

int rpc_status(void *context, const DejavuRpcRequest *request, DejavuRpcResponse *response,
               char *error, size_t error_size) {
    auto *state = static_cast<CaptureState *>(context);
    if (dejavu_example::rpc_is_cancelled(request)) {
        return dejavu_example::rpc_reply_error(state->host, response, "capture.cancelled",
                                               "Request cancelled", false, error, error_size);
    }
    return dejavu_example::rpc_reply(state->host, response, true, status_json(state), error,
                                     error_size);
}

int rpc_start(void *context, const DejavuRpcRequest *request, DejavuRpcResponse *response,
              char *error, size_t error_size) {
    auto *state = static_cast<CaptureState *>(context);
    if (dejavu_example::rpc_is_cancelled(request)) {
        return dejavu_example::rpc_reply_error(state->host, response, "capture.cancelled",
                                               "Request cancelled", false, error, error_size);
    }
    std::string path;
    int64_t transport = 0;
    std::string endpoint;
    if (request != nullptr && request->params_json != nullptr && request->params_size > 0) {
        dejavu_example::json_get_string(request->params_json, request->params_size, "path", &path);
        dejavu_example::json_get_int64(request->params_json, request->params_size, "transport",
                                       &transport);
        dejavu_example::json_get_string(request->params_json, request->params_size, "endpoint",
                                        &endpoint);
    }
    if (transport < 0 || transport > DEJAVU_STREAM_TRANSPORT_KCP) {
        return dejavu_example::rpc_reply_error(state->host, response, "capture.invalid_argument",
                                               "transport must be 0, 1 (tcp), 2 (udp) or 3 (kcp)",
                                               false, error, error_size);
    }
    std::string start_error;
    if (!start_capture(state, path, static_cast<uint32_t>(transport), endpoint, &start_error,
                       nullptr)) {
        const char *code = start_error == "capture is already active" ? "capture.already_active"
                                                                      : "capture.start_failed";
        return dejavu_example::rpc_reply_error(state->host, response, code, start_error, false,
                                               error, error_size);
    }
    return dejavu_example::rpc_reply(state->host, response, true, status_json(state), error,
                                     error_size);
}

int rpc_stop(void *context, const DejavuRpcRequest *request, DejavuRpcResponse *response,
             char *error, size_t error_size) {
    auto *state = static_cast<CaptureState *>(context);
    if (dejavu_example::rpc_is_cancelled(request)) {
        return dejavu_example::rpc_reply_error(state->host, response, "capture.cancelled",
                                               "Request cancelled", false, error, error_size);
    }
    std::string stop_error;
    if (!stop_capture(state, &stop_error)) {
        return dejavu_example::rpc_reply_error(state->host, response, "capture.stop_failed",
                                               stop_error, false, error, error_size);
    }
    return dejavu_example::rpc_reply(state->host, response, true, status_json(state), error,
                                     error_size);
}

int rpc_recent(void *context, const DejavuRpcRequest *request, DejavuRpcResponse *response,
               char *error, size_t error_size) {
    auto *state = static_cast<CaptureState *>(context);
    if (dejavu_example::rpc_is_cancelled(request)) {
        return dejavu_example::rpc_reply_error(state->host, response, "capture.cancelled",
                                               "Request cancelled", false, error, error_size);
    }
    int64_t limit = static_cast<int64_t>(kDefaultRpcRecentLimit);
    bool clear = false;
    if (request != nullptr && request->params_json != nullptr && request->params_size > 0) {
        dejavu_example::json_get_int64(request->params_json, request->params_size, "limit", &limit);
        dejavu_example::json_get_bool(request->params_json, request->params_size, "clear", &clear);
    }
    if (limit < 1) {
        limit = 1;
    } else if (static_cast<size_t>(limit) > kMaxRpcRecentLimit) {
        limit = static_cast<int64_t>(kMaxRpcRecentLimit);
    }

    std::string json;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const size_t count = state->recent.size();
        const size_t take = count < static_cast<size_t>(limit) ? count : static_cast<size_t>(limit);
        const size_t start_index = count - take;
        json = "{\"active\":";
        json += state->active ? "true" : "false";
        json += ",\"route\":\"" + dejavu_example::json_escape(state->rpc_route) + "\"";
        json += ",\"total_buffered\":" + std::to_string(count);
        json += ",\"dropped_records\":" + std::to_string(state->dropped_records);
        json += ",\"records\":[";
        for (size_t i = 0; i < take; ++i) {
            const CaptureRecord &rec = state->recent[start_index + i];
            if (i > 0) {
                json.push_back(',');
            }
            json += "{\"seq\":" + std::to_string(rec.seq);
            json += ",\"dir\":\"";
            json += (rec.direction == 'O' ? "OUT" : "IN");
            json += "\",\"length\":" + std::to_string(rec.length);
            json += ",\"preview\":\"" + dejavu_example::json_escape(rec.preview) + "\"}";
        }
        json += "]}";
        if (clear) {
            state->recent.clear();
        }
    }
    return dejavu_example::rpc_reply(state->host, response, true, json, error, error_size);
}

int rpc_clear(void *context, const DejavuRpcRequest *request, DejavuRpcResponse *response,
              char *error, size_t error_size) {
    auto *state = static_cast<CaptureState *>(context);
    if (dejavu_example::rpc_is_cancelled(request)) {
        return dejavu_example::rpc_reply_error(state->host, response, "capture.cancelled",
                                               "Request cancelled", false, error, error_size);
    }
    size_t cleared = 0;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        cleared = state->recent.size();
        state->recent.clear();
    }
    return dejavu_example::rpc_reply(state->host, response, true,
                                     "{\"cleared\":" + std::to_string(cleared) + "}", error,
                                     error_size);
}

int rpc_probe(void *context, const DejavuRpcRequest *request, DejavuRpcResponse *response,
              char *error, size_t error_size) {
    auto *state = static_cast<CaptureState *>(context);
    if (dejavu_example::rpc_is_cancelled(request)) {
        return dejavu_example::rpc_reply_error(state->host, response, "capture.cancelled",
                                               "Request cancelled", false, error, error_size);
    }
    int64_t transport = 0;
    std::string endpoint;
    if (request != nullptr && request->params_json != nullptr && request->params_size > 0) {
        dejavu_example::json_get_int64(request->params_json, request->params_size, "transport",
                                       &transport);
        dejavu_example::json_get_string(request->params_json, request->params_size, "endpoint",
                                        &endpoint);
    }
    if (transport < DEJAVU_STREAM_TRANSPORT_TCP || transport > DEJAVU_STREAM_TRANSPORT_KCP ||
        endpoint.empty()) {
        return dejavu_example::rpc_reply_error(state->host, response, "capture.invalid_argument",
                                               "probe requires transport (1/2/3) and endpoint",
                                               false, error, error_size);
    }
    std::string probe_error;
    if (!run_probe(state, static_cast<uint32_t>(transport), endpoint, &probe_error)) {
        return dejavu_example::rpc_reply_error(state->host, response, "capture.probe_failed",
                                               probe_error, false, error, error_size);
    }
    return dejavu_example::rpc_reply(state->host, response, true, "{\"sent\":true}", error,
                                     error_size);
}

int lua_start(dejavu_control_session *, void *context, const dejavu_value *arguments,
              size_t argument_count, dejavu_value *result, char *error, size_t error_size) {
    auto *state = static_cast<CaptureState *>(context);
    if (state == nullptr ||
        (argument_count != 0 && argument_count != 1 && argument_count != 3)) {
        std::snprintf(
            error, error_size,
            "capture.start expects (), <path>, or <path>, <1=tcp|2=udp|3=kcp>, <endpoint>");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::string path;
    if (argument_count >= 1) {
        if (arguments[0].type != DEJAVU_VALUE_STRING || arguments[0].string_value == nullptr) {
            std::snprintf(error, error_size, "capture.start path must be a string");
            return DEJAVU_ERROR_INVALID_ARGUMENT;
        }
        path.assign(arguments[0].string_value, arguments[0].string_size);
    }
    uint32_t transport = 0;
    std::string endpoint;
    if (argument_count == 3) {
        if (arguments[1].type != DEJAVU_VALUE_INTEGER ||
            arguments[1].integer_value < DEJAVU_STREAM_TRANSPORT_TCP ||
            arguments[1].integer_value > DEJAVU_STREAM_TRANSPORT_KCP ||
            arguments[2].type != DEJAVU_VALUE_STRING || arguments[2].string_value == nullptr) {
            std::snprintf(error, error_size, "transport must be 1 (tcp), 2 (udp) or 3 (kcp)");
            return DEJAVU_ERROR_INVALID_ARGUMENT;
        }
        transport = static_cast<uint32_t>(arguments[1].integer_value);
        endpoint.assign(arguments[2].string_value, arguments[2].string_size);
        if (endpoint.empty()) {
            std::snprintf(error, error_size, "stream endpoint is required");
            return DEJAVU_ERROR_INVALID_ARGUMENT;
        }
    }
    std::string start_error;
    std::string summary;
    if (!start_capture(state, path, transport, endpoint, &start_error, &summary)) {
        std::snprintf(error, error_size, "%s", start_error.c_str());
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    set_result_string(state, result, summary);
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

int lua_probe(dejavu_control_session *, void *context, const dejavu_value *arguments,
              size_t argument_count, dejavu_value *result, char *error, size_t error_size) {
    auto *state = static_cast<CaptureState *>(context);
    if (state == nullptr || argument_count != 2 || arguments[0].type != DEJAVU_VALUE_INTEGER ||
        arguments[1].type != DEJAVU_VALUE_STRING || arguments[1].string_value == nullptr) {
        std::snprintf(error, error_size, "capture.probe expects transport (1/2/3) and endpoint");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    const uint32_t transport = static_cast<uint32_t>(arguments[0].integer_value);
    const std::string endpoint(arguments[1].string_value, arguments[1].string_size);
    std::string probe_error;
    if (!run_probe(state, transport, endpoint, &probe_error)) {
        std::snprintf(error, error_size, "%s", probe_error.c_str());
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    set_result_string(state, result, "stream probe sent");
    return DEJAVU_OK;
}

void unregister_rpc(CaptureState *state) {
    if (state != nullptr && state->rpc != nullptr && state->rpc_service != 0 &&
        state->rpc->unregister_service != nullptr) {
        char rpc_error[128] = {};
        state->rpc->unregister_service(state->host->host_context, state->rpc_service, rpc_error,
                                       sizeof(rpc_error));
        state->rpc_service = 0;
    }
}

bool register_rpc(CaptureState *state, char *error, size_t error_size) {
    if (state == nullptr || state->host == nullptr || state->host->get_interface == nullptr) {
        std::snprintf(error, error_size, "RPC interface v1 required");
        return false;
    }
    const auto *rpc = static_cast<const DejavuRpcInterface *>(state->host->get_interface(
        state->host->host_context, "rpc", DEJAVU_RPC_INTERFACE_VERSION));
    if (rpc == nullptr || rpc->version != DEJAVU_RPC_INTERFACE_VERSION ||
        rpc->struct_size < sizeof(*rpc) || rpc->register_service == nullptr ||
        rpc->unregister_service == nullptr) {
        std::snprintf(error, error_size, "RPC interface v1 required");
        return false;
    }
    state->rpc = rpc;
    const char *routes[] = {state->rpc_route.c_str()};
    static const DejavuRpcMethod methods[] = {
        {"status", rpc_status},
        {"start", rpc_start},
        {"stop", rpc_stop},
        {"recent", rpc_recent},
        {"clear", rpc_clear},
        {"probe", rpc_probe},
    };
    const DejavuRpcServiceDescriptor descriptor = {
        sizeof(DejavuRpcServiceDescriptor),
        "capture",
        state->process_name.c_str(),
        routes,
        1,
        methods,
        sizeof(methods) / sizeof(methods[0]),
        state,
        nullptr,
    };
    if (rpc->register_service(state->host->host_context, &descriptor, &state->rpc_service, error,
                              error_size) != DEJAVU_RPC_OK) {
        state->rpc_service = 0;
        return false;
    }
    return true;
}

int plugin_initialize(const DejavuPluginHost *host, void **plugin_state, char *error,
                      size_t error_size) {
    if (plugin_state != nullptr) {
        *plugin_state = nullptr;
    }
    if (host == nullptr || plugin_state == nullptr ||
        host->abi_version != DEJAVU_PLUGIN_ABI_VERSION ||
        host->struct_size <
            offsetof(DejavuPluginHost, get_interface) + sizeof(host->get_interface) ||
        host->get_interface == nullptr || host->alloc == nullptr || host->free == nullptr) {
        std::snprintf(error, error_size, "incompatible plugin host ABI");
        return -1;
    }
    auto *state = new CaptureState();
    state->host = host;
    state->process_name = dejavu_example::current_process_name();
    state->rpc_route = dejavu_example::make_process_route("capture");
    state->module = static_cast<const DejavuModuleInterface *>(
        host->get_interface(host->host_context, "module", DEJAVU_MODULE_INTERFACE_VERSION));
    state->hook = static_cast<const DejavuHookInterface *>(
        host->get_interface(host->host_context, "hook", DEJAVU_HOOK_INTERFACE_VERSION));
    state->stream = static_cast<const DejavuStreamInterface *>(
        host->get_interface(host->host_context, "stream", DEJAVU_STREAM_INTERFACE_VERSION));
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
        {"probe", lua_probe},
    };
    for (const auto &registration : registrations) {
        if (host->register_function(host->host_context, "capture", registration.name,
                                    registration.function, state, error, error_size) != DEJAVU_OK) {
            delete state;
            return -1;
        }
    }
    if (!register_rpc(state, error, error_size)) {
        delete state;
        return -1;
    }
    *plugin_state = state;
    return 0;
}

void plugin_unload_prepare(void *plugin_state) {
    auto *state = static_cast<CaptureState *>(plugin_state);
    if (state == nullptr) {
        return;
    }
    unregister_rpc(state);
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
    unregister_rpc(state);
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
        "1.1.0",
        DEJAVU_PLUGIN_FLAG_UNLOAD_SAFE,
        plugin_initialize,
        nullptr,
        plugin_shutdown,
        DEJAVU_PLUGIN_CAP_MODULE | DEJAVU_PLUGIN_CAP_HOOK | DEJAVU_PLUGIN_CAP_STREAM |
            DEJAVU_PLUGIN_CAP_RPC,
        plugin_unload_prepare,
    };
    return &descriptor;
}
