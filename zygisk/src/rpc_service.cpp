#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "rpc_service.h"
#include "cJSON.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
constexpr size_t kMaxFrame = 1024 * 1024;
constexpr size_t kMaxQueued = 16;
constexpr size_t kMaxServices = 16;
constexpr uint64_t kMaxBudget = 86400000;

uint64_t monotonic_ms() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}
void set_error(std::string *error, const char *text) {
    if (error)
        *error = text;
}
bool bounded_text(const char *text, size_t maximum, bool empty = false) {
    if (!text || (!empty && !*text) || strnlen(text, maximum + 1) > maximum)
        return false;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p)
        if (*p < 32 || *p == 127)
            return false;
    return true;
}
bool identifier(const char *text) {
    if (!bounded_text(text, 128))
        return false;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '_' || *p == '-' || *p == '.'))
            return false;
    return true;
}
bool copy_routes(const char *const *input, size_t count, std::vector<std::string> *output) {
    if (count > 64 || (count && !input))
        return false;
    std::set<std::string> unique;
    for (size_t i = 0; i < count; ++i) {
        if (!bounded_text(input[i], 128) || !unique.emplace(input[i]).second)
            return false;
        output->emplace_back(input[i]);
    }
    return true;
}
cJSON *parse_json(const std::string &text) {
    // Reject embedded NULs before cJSON's zero-terminated string representation loses them.
    for (size_t i = 0; i < text.size(); ++i) {
        if (!text[i])
            return nullptr;
        if (text[i] == '\\' && i + 1 < text.size()) {
            if (text[i + 1] == 'u' && text.compare(i + 2, 4, "0000") == 0)
                return nullptr;
            ++i;
        }
    }
    return cJSON_ParseWithLengthOpts(text.c_str(), text.size() + 1, nullptr, true);
}
std::string encode(cJSON *json) {
    char *raw = cJSON_PrintUnformatted(json);
    if (!raw)
        return {};
    std::string result(raw);
    free(raw);
    return result;
}
std::string error_response(const std::string &id, const std::string &route, const char *code,
                           const char *message) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "response");
    cJSON_AddStringToObject(root, "id", id.c_str());
    cJSON_AddStringToObject(root, "route", route.c_str());
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON *error = cJSON_AddObjectToObject(root, "error");
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddBoolToObject(error, "retryable", false);
    std::string result = encode(root);
    cJSON_Delete(root);
    return result;
}
bool endpoint_valid(const std::string &endpoint) {
    if (endpoint.compare(0, 9, "abstract:") == 0) {
#ifdef __APPLE__
        return false;
#else
        const std::string name = endpoint.substr(9);
        return identifier(name.c_str()) && name.size() < sizeof(sockaddr_un::sun_path) - 1;
#endif
    }
    return endpoint.compare(0, 6, "unix:/") == 0 && endpoint.size() > 6 &&
           endpoint.size() - 5 < sizeof(sockaddr_un::sun_path) &&
           bounded_text(endpoint.c_str(), 160);
}
bool trusted_peer(int fd) {
    uid_t uid = static_cast<uid_t>(-1);
#ifdef __APPLE__
    gid_t gid;
    if (getpeereid(fd, &uid, &gid) != 0)
        return false;
#else
    ucred credentials{};
    socklen_t size = sizeof(credentials);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0 ||
        size != sizeof(credentials))
        return false;
    uid = credentials.uid;
#endif
#ifdef __ANDROID__
    return uid == 0;
#else
    return uid == geteuid();
#endif
}
} // namespace

struct RpcService::Service {
    struct Task {
        std::string id, route, method, params;
        uint64_t deadline = 0, generation = 0;
        std::atomic<bool> cancelled{false};
        static int is_cancelled(void *opaque) {
            auto *task = static_cast<Task *>(opaque);
            return task->cancelled.load() || monotonic_ms() >= task->deadline;
        }
    };
    uint64_t owner;
    Invoke invoke;
    std::string name, app, endpoint;
    std::vector<std::string> routes;
    std::map<std::string, DejavuRpcHandler> methods;
    void *context;
    std::mutex mutex;
    std::condition_variable changed;
    std::thread io, dispatch;
    std::thread::id io_id, dispatch_id;
    std::mutex join_mutex;
    bool active = false, stopping = false, reconnect = false;
    bool io_done = false, dispatch_done = false, online = false;
    uint64_t generation = 1;
    int fd = -1;
    std::deque<std::shared_ptr<Task>> queued;
    std::map<std::string, std::shared_ptr<Task>> pending;
    std::deque<std::string> outgoing;

    Service(uint64_t token, const DejavuRpcServiceDescriptor &descriptor, Invoke call,
            std::vector<std::string> copied_routes)
        : owner(token), invoke(std::move(call)), name(descriptor.service_name), app(descriptor.app),
          endpoint(descriptor.endpoint ? descriptor.endpoint : DEJAVU_RPC_DEFAULT_ENDPOINT),
          routes(std::move(copied_routes)), context(descriptor.context) {
        for (size_t i = 0; i < descriptor.method_count; ++i)
            methods.emplace(descriptor.methods[i].name, descriptor.methods[i].callback);
    }
    ~Service() {
        stop();
        join();
    }
    void start() {
        dispatch = std::thread([this] { dispatch_loop(); });
        dispatch_id = dispatch.get_id();
        try {
            io = std::thread([this] { io_loop(); });
            io_id = io.get_id();
        } catch (...) {
            stop();
            dispatch.join();
            throw;
        }
    }
    void cancel_locked() {
        ++generation;
        for (auto &item : pending)
            item.second->cancelled.store(true);
        pending.clear();
        queued.clear();
        outgoing.clear();
        online = false;
        changed.notify_all();
    }
    void stop() {
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
        cancel_locked();
        if (fd >= 0)
            shutdown(fd, SHUT_RDWR);
    }
    bool drain(std::chrono::steady_clock::time_point until) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_until(lock, until, [this] { return io_done && dispatch_done; });
    }
    bool own_thread() const {
        return std::this_thread::get_id() == io_id || std::this_thread::get_id() == dispatch_id;
    }
    void join() {
        std::lock_guard<std::mutex> lock(join_mutex);
        if (io.joinable())
            io.join();
        if (dispatch.joinable())
            dispatch.join();
    }
    bool interrupted(uint64_t expected) {
        std::lock_guard<std::mutex> lock(mutex);
        return stopping || reconnect || generation != expected;
    }
    bool wait_socket(int socket, short events, uint64_t expected, uint64_t until) {
        while (monotonic_ms() < until && !interrupted(expected)) {
            pollfd descriptor{socket, events, 0};
            int timeout = static_cast<int>(std::min<uint64_t>(50, until - monotonic_ms()));
            if (timeout < 0)
                return false;
            int result = poll(&descriptor, 1, timeout);
            if (result > 0)
                return (descriptor.revents & events) != 0 &&
                       !(descriptor.revents & (POLLERR | POLLNVAL));
            if (result < 0 && errno != EINTR)
                return false;
        }
        return false;
    }
    bool write_line(int socket, const std::string &text, uint64_t expected) {
        if (text.empty() || text.size() > kMaxFrame)
            return false;
        const std::string bytes = text + '\n';
        size_t sent = 0;
        const uint64_t until = monotonic_ms() + 500;
        while (sent < bytes.size() && !interrupted(expected)) {
#ifdef MSG_NOSIGNAL
            ssize_t count = send(socket, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
#else
            ssize_t count = send(socket, bytes.data() + sent, bytes.size() - sent, 0);
#endif
            if (count > 0) {
                sent += static_cast<size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
                wait_socket(socket, POLLOUT, expected, until))
                continue;
            return false;
        }
        return sent == bytes.size();
    }
    int connect_socket(uint64_t expected) {
        int socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (socket < 0)
            return -1;
        fcntl(socket, F_SETFD, FD_CLOEXEC);
        fcntl(socket, F_SETFL, fcntl(socket, F_GETFL, 0) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        socklen_t length;
        if (endpoint.compare(0, 9, "abstract:") == 0) {
            const std::string text = endpoint.substr(9);
            memcpy(address.sun_path + 1, text.data(), text.size());
            length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + text.size());
        } else {
            const std::string text = endpoint.substr(5);
            memcpy(address.sun_path, text.c_str(), text.size() + 1);
            length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + text.size() + 1);
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping || reconnect || generation != expected) {
                close(socket);
                return -1;
            }
            fd = socket;
        }
        int result = connect(socket, reinterpret_cast<sockaddr *>(&address), length);
        bool connected = result == 0;
        if (!connected && (errno == EINPROGRESS || errno == EAGAIN) &&
            wait_socket(socket, POLLOUT, expected, monotonic_ms() + 2000)) {
            int error = 0;
            socklen_t size = sizeof(error);
            connected = getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0;
        }
        if (connected && !interrupted(expected) && trusted_peer(socket))
            return socket;
        {
            std::lock_guard<std::mutex> lock(mutex);
            fd = -1;
        }
        close(socket);
        return -1;
    }
    std::string hello() {
        std::lock_guard<std::mutex> lock(mutex);
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "hello");
        cJSON_AddStringToObject(root, "app", app.c_str());
        cJSON_AddStringToObject(root, "plugin", name.c_str());
        cJSON_AddNumberToObject(root, "version", 1);
        cJSON *list = cJSON_AddArrayToObject(root, "routes");
        for (const auto &route : routes)
            cJSON_AddItemToArray(list, cJSON_CreateString(route.c_str()));
        std::string result = encode(root);
        cJSON_Delete(root);
        return result;
    }
    void enqueue_error_locked(const std::string &id, const std::string &route, const char *code,
                              const char *message) {
        if (outgoing.size() >= kMaxQueued + 1) {
            reconnect = true;
            if (fd >= 0)
                shutdown(fd, SHUT_RDWR);
            return;
        }
        outgoing.push_back(error_response(id, route, code, message));
    }
    bool handle_line(const std::string &line, uint64_t expected) {
        cJSON *root = parse_json(line);
        if (!cJSON_IsObject(root)) {
            cJSON_Delete(root);
            return false;
        }
        cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
        if (!cJSON_IsString(type)) {
            cJSON_Delete(root);
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (generation != expected || stopping || reconnect) {
            cJSON_Delete(root);
            return false;
        }
        const std::string kind(type->valuestring);
        if (kind == "hello_ok") {
            cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
            if (online || !cJSON_IsNumber(version) || version->valuedouble != 1) {
                cJSON_Delete(root);
                return false;
            }
            online = true;
        } else if (kind == "ping" && online) {
            if (outgoing.size() < kMaxQueued + 1)
                outgoing.emplace_back("{\"type\":\"pong\"}");
            else
                reconnect = true;
        } else if (kind == "cancel" && online) {
            cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
            cJSON *route = cJSON_GetObjectItemCaseSensitive(root, "route");
            if (!cJSON_IsString(id) || !cJSON_IsString(route)) {
                cJSON_Delete(root);
                return false;
            }
            auto item = pending.find(id->valuestring);
            if (item != pending.end() && item->second->route == route->valuestring) {
                item->second->cancelled.store(true);
                pending.erase(item);
            }
        } else if (kind == "request" && online) {
            cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
            cJSON *route = cJSON_GetObjectItemCaseSensitive(root, "route");
            cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
            cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
            if (!cJSON_IsString(id) || !bounded_text(id->valuestring, 128) ||
                !cJSON_IsString(route) || !bounded_text(route->valuestring, 128) ||
                !cJSON_IsString(method) || !identifier(method->valuestring)) {
                cJSON_Delete(root);
                return false;
            }
            if (std::find(routes.begin(), routes.end(), route->valuestring) == routes.end())
                enqueue_error_locked(id->valuestring, route->valuestring, "rpc.route_mismatch",
                                     "Route is not registered by this service");
            else if (!methods.count(method->valuestring))
                enqueue_error_locked(id->valuestring, route->valuestring, "rpc.method_not_found",
                                     "Method is not registered by this service");
            else if (pending.count(id->valuestring))
                enqueue_error_locked(id->valuestring, route->valuestring, "rpc.duplicate_id",
                                     "Request ID is already pending");
            else if (queued.size() >= kMaxQueued)
                enqueue_error_locked(id->valuestring, route->valuestring, "rpc.busy",
                                     "Service request queue is full");
            else {
                auto task = std::make_shared<Task>();
                task->id = id->valuestring;
                task->route = route->valuestring;
                task->method = method->valuestring;
                task->params = params ? encode(params) : "{}";
                task->generation = expected;
                const uint64_t now = monotonic_ms();
                task->deadline = now + 60000;
                cJSON *deadline = cJSON_GetObjectItemCaseSensitive(root, "deadline_monotonic_ms");
                if (deadline) {
                    if (!cJSON_IsNumber(deadline) || !std::isfinite(deadline->valuedouble) ||
                        deadline->valuedouble < 0 ||
                        deadline->valuedouble > static_cast<double>(now + kMaxBudget) ||
                        deadline->valuedouble !=
                            static_cast<double>(static_cast<uint64_t>(deadline->valuedouble))) {
                        cJSON_Delete(root);
                        return false;
                    }
                    task->deadline = static_cast<uint64_t>(deadline->valuedouble);
                }
                pending.emplace(task->id, task);
                queued.push_back(task);
                changed.notify_all();
            }
        } else {
            cJSON_Delete(root);
            return false;
        }
        cJSON_Delete(root);
        return !reconnect;
    }
    void run_connection(int socket, uint64_t expected) {
        if (!write_line(socket, hello(), expected))
            return;
        const uint64_t handshake_until = monotonic_ms() + 2000;
        std::string input;
        while (!interrupted(expected)) {
            std::deque<std::string> replies;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!online && monotonic_ms() >= handshake_until)
                    return;
                for (auto it = pending.begin(); it != pending.end();) {
                    if (monotonic_ms() >= it->second->deadline) {
                        it->second->cancelled.store(true);
                        it = pending.erase(it);
                    } else
                        ++it;
                }
                replies.swap(outgoing);
            }
            for (const auto &reply : replies)
                if (!write_line(socket, reply, expected))
                    return;
            pollfd descriptor{socket, POLLIN, 0};
            int ready = poll(&descriptor, 1, 25);
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready < 0 || (ready && descriptor.revents & (POLLERR | POLLNVAL)))
                return;
            if (!ready)
                continue;
            char bytes[8192];
            ssize_t count = recv(socket, bytes, sizeof(bytes), 0);
            if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                continue;
            if (count <= 0)
                return;
            input.append(bytes, static_cast<size_t>(count));
            size_t newline;
            while ((newline = input.find('\n')) != std::string::npos) {
                if (newline > kMaxFrame)
                    return;
                std::string line = input.substr(0, newline);
                input.erase(0, newline + 1);
                if (!handle_line(line, expected))
                    return;
            }
            if (input.size() > kMaxFrame)
                return;
        }
    }
    void io_loop() {
        for (;;) {
            uint64_t expected;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [this] { return stopping || (active && !routes.empty()); });
                if (stopping)
                    break;
                reconnect = false;
                expected = generation;
            }
            int socket = connect_socket(expected);
            if (socket >= 0) {
                run_connection(socket, expected);
                std::lock_guard<std::mutex> lock(mutex);
                fd = -1;
                close(socket);
                cancel_locked();
            }
            std::unique_lock<std::mutex> lock(mutex);
            if (stopping)
                break;
            changed.wait_for(lock, std::chrono::seconds(1),
                             [this] { return stopping || reconnect || routes.empty(); });
        }
        std::lock_guard<std::mutex> lock(mutex);
        io_done = true;
        changed.notify_all();
    }
    void dispatch_loop() {
        for (;;) {
            std::shared_ptr<Task> task;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [this] { return stopping || !queued.empty(); });
                if (stopping)
                    break;
                task = queued.front();
                queued.pop_front();
            }
            if (Task::is_cancelled(task.get()))
                continue;
            DejavuRpcRequest request{
                sizeof(DejavuRpcRequest), task->id.c_str(),     task->route.c_str(),
                task->method.c_str(),     task->params.c_str(), task->params.size(),
                task->deadline,           task.get(),           &Task::is_cancelled};
            DejavuRpcResponse response{sizeof(DejavuRpcResponse), 0, nullptr, 0};
            char error[256]{};
            int result = DEJAVU_RPC_ERROR;
            try {
                result = invoke(owner, methods.at(task->method), context, &request, &response,
                                error, sizeof(error));
            } catch (...) {
                result = DEJAVU_RPC_ERROR;
            }
            std::unique_ptr<char, decltype(&free)> payload(response.payload_json, &free);
            std::string reply;
            if (!Task::is_cancelled(task.get())) {
                cJSON *value = nullptr;
                if (result == DEJAVU_RPC_OK && response.struct_size >= sizeof(DejavuRpcResponse) &&
                    (response.ok == 0 || response.ok == 1) && response.payload_json &&
                    response.payload_size <= kMaxFrame)
                    value = parse_json(std::string(response.payload_json, response.payload_size));
                bool valid = value != nullptr;
                if (valid && response.ok == 0) {
                    cJSON *code = cJSON_GetObjectItemCaseSensitive(value, "code");
                    cJSON *message = cJSON_GetObjectItemCaseSensitive(value, "message");
                    cJSON *retryable = cJSON_GetObjectItemCaseSensitive(value, "retryable");
                    valid = cJSON_IsObject(value) && cJSON_IsString(code) &&
                            bounded_text(code->valuestring, 128) &&
                            strncmp(code->valuestring, "rpc.", 4) != 0 && cJSON_IsString(message) &&
                            bounded_text(message->valuestring, 4096, true) &&
                            (!retryable || cJSON_IsBool(retryable));
                }
                if (valid) {
                    cJSON *root = cJSON_CreateObject();
                    cJSON_AddStringToObject(root, "type", "response");
                    cJSON_AddStringToObject(root, "id", task->id.c_str());
                    cJSON_AddStringToObject(root, "route", task->route.c_str());
                    cJSON_AddBoolToObject(root, "ok", response.ok);
                    cJSON_AddItemToObject(root, response.ok ? "result" : "error", value);
                    reply = encode(root);
                    cJSON_Delete(root);
                    if (reply.size() > kMaxFrame)
                        reply = error_response(task->id, task->route, "rpc.invalid_response",
                                               "Handler response exceeds frame limit");
                } else {
                    cJSON_Delete(value);
                    reply = error_response(
                        task->id, task->route,
                        result == DEJAVU_RPC_OK ? "rpc.invalid_response" : "rpc.handler_failed",
                        result == DEJAVU_RPC_OK ? "Handler returned an invalid response"
                                                : "Business handler failed");
                }
            }
            std::lock_guard<std::mutex> lock(mutex);
            auto current = pending.find(task->id);
            if (current != pending.end() && current->second == task)
                pending.erase(current);
            if (!stopping && online && generation == task->generation &&
                !Task::is_cancelled(task.get()) && !reply.empty()) {
                if (outgoing.size() < kMaxQueued + 1)
                    outgoing.push_back(std::move(reply));
                else {
                    reconnect = true;
                    if (fd >= 0)
                        shutdown(fd, SHUT_RDWR);
                }
            }
        }
        std::lock_guard<std::mutex> lock(mutex);
        dispatch_done = true;
        changed.notify_all();
    }
};

RpcService::RpcService(Invoke invoke) : invoke_(std::move(invoke)) {
    if (!invoke_)
        invoke_ = [](uint64_t, DejavuRpcHandler callback, void *context,
                     const DejavuRpcRequest *request, DejavuRpcResponse *response, char *error,
                     size_t error_size) {
            return callback(context, request, response, error, error_size);
        };
}
RpcService::~RpcService() {
    stop_and_join();
}
void RpcService::stop_and_join() {
    std::lock_guard<std::mutex> teardown_lock(teardown_mutex_);
    std::vector<std::shared_ptr<Service>> services;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutting_down_ = true;
        active_owners_.clear();
        for (auto &item : services_) {
            retired_owners_.insert(item.second->owner);
            services.push_back(item.second);
        }
    }
    for (auto &service : services)
        service->stop();
    for (auto &service : services)
        service->join();
    std::lock_guard<std::mutex> lock(mutex_);
    services_.clear();
}
std::shared_ptr<RpcService::Service> RpcService::find(uint64_t owner, uint64_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = services_.find(handle);
    return found == services_.end() || found->second->owner != owner ? nullptr : found->second;
}
int RpcService::register_service(uint64_t owner, const DejavuRpcServiceDescriptor *descriptor,
                                 uint64_t *handle, std::string *error) {
    if (!owner || !descriptor || !handle || descriptor->struct_size < sizeof(*descriptor) ||
        !identifier(descriptor->service_name) || !bounded_text(descriptor->app, 256) ||
        descriptor->method_count == 0 || descriptor->method_count > 64 || !descriptor->methods ||
        !endpoint_valid(descriptor->endpoint ? descriptor->endpoint
                                             : DEJAVU_RPC_DEFAULT_ENDPOINT)) {
        set_error(error, "Invalid RPC service descriptor or Unix endpoint");
        return DEJAVU_RPC_ERROR;
    }
    std::vector<std::string> routes;
    if (!copy_routes(descriptor->routes, descriptor->route_count, &routes)) {
        set_error(error, "Invalid RPC routes");
        return DEJAVU_RPC_ERROR;
    }
    std::set<std::string> names;
    for (size_t i = 0; i < descriptor->method_count; ++i)
        if (!identifier(descriptor->methods[i].name) || !descriptor->methods[i].callback ||
            !names.emplace(descriptor->methods[i].name).second) {
            set_error(error, "Invalid or duplicate RPC method");
            return DEJAVU_RPC_ERROR;
        }
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutting_down_ || retired_owners_.count(owner) || services_.size() >= kMaxServices) {
        set_error(error, "RPC owner retired or service limit reached");
        return DEJAVU_RPC_ERROR;
    }
    for (const auto &item : services_)
        if (item.second->owner == owner && item.second->name == descriptor->service_name) {
            set_error(error, "RPC service name already registered for owner");
            return DEJAVU_RPC_ERROR;
        }
    auto service = std::make_shared<Service>(owner, *descriptor, invoke_, std::move(routes));
    service->active = active_owners_.count(owner) != 0;
    try {
        service->start();
    } catch (...) {
        set_error(error, "Unable to start RPC worker");
        return DEJAVU_RPC_ERROR;
    }
    const uint64_t token = next_handle_++;
    services_.emplace(token, std::move(service));
    *handle = token;
    return DEJAVU_RPC_OK;
}
int RpcService::update_routes(uint64_t owner, uint64_t handle, const char *const *input,
                              size_t count, std::string *error) {
    std::vector<std::string> routes;
    if (!copy_routes(input, count, &routes)) {
        set_error(error, "Invalid RPC routes");
        return DEJAVU_RPC_ERROR;
    }
    auto service = find(owner, handle);
    if (!service) {
        set_error(error, "RPC service is not owned by caller");
        return DEJAVU_RPC_ERROR;
    }
    std::lock_guard<std::mutex> lock(service->mutex);
    if (service->stopping) {
        set_error(error, "RPC service is retiring");
        return DEJAVU_RPC_ERROR;
    }
    if (service->routes == routes)
        return DEJAVU_RPC_OK;
    service->routes = std::move(routes);
    service->cancel_locked();
    service->reconnect = true;
    if (service->fd >= 0)
        shutdown(service->fd, SHUT_RDWR);
    service->changed.notify_all();
    return DEJAVU_RPC_OK;
}
int RpcService::unregister_service(uint64_t owner, uint64_t handle, std::string *error) {
    auto service = find(owner, handle);
    if (!service) {
        set_error(error, "RPC service is not owned by caller");
        return DEJAVU_RPC_ERROR;
    }
    service->stop();
    if (service->own_thread() ||
        !service->drain(std::chrono::steady_clock::now() + std::chrono::milliseconds(250))) {
        set_error(error, "RPC callback is still draining");
        return DEJAVU_RPC_BUSY;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = services_.find(handle);
        if (found != services_.end() && found->second == service)
            services_.erase(found);
    }
    service->join();
    return DEJAVU_RPC_OK;
}
void RpcService::activate_owner(uint64_t owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!owner || shutting_down_ || retired_owners_.count(owner))
        return;
    active_owners_.insert(owner);
    for (auto &item : services_)
        if (item.second->owner == owner) {
            std::lock_guard<std::mutex> service_lock(item.second->mutex);
            item.second->active = true;
            item.second->changed.notify_all();
        }
}
bool RpcService::retire_owner(uint64_t owner) {
    std::vector<std::pair<uint64_t, std::shared_ptr<Service>>> owned;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        retired_owners_.insert(owner);
        active_owners_.erase(owner);
        for (auto &item : services_)
            if (item.second->owner == owner)
                owned.push_back(item);
    }
    for (auto &item : owned)
        item.second->stop();
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    bool complete = true;
    for (auto &item : owned) {
        if (item.second->own_thread() || !item.second->drain(until)) {
            complete = false;
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto found = services_.find(item.first);
            if (found != services_.end() && found->second == item.second)
                services_.erase(found);
        }
        item.second->join();
    }
    return complete;
}
