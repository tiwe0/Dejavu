#include "dejavu_plugin.h"
#include "cJSON.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <new>
#include <string>
#include <vector>

#define LOG_TAG "TikTokAgentPlugin"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr const char *kVersion = "0.1.0";
constexpr const char *kRouterHost = "127.0.0.1";
constexpr int kRouterPort = 39111;
constexpr size_t kMaxLine = 8 * 1024 * 1024;
constexpr size_t kMaxFile = 8 * 1024 * 1024;

struct AgentState {
    const DejavuPluginHost *host = nullptr;
    pthread_t thread{};
    bool thread_started = false;
    std::atomic<bool> stop{false};
    std::atomic<int> router_fd{-1};
};

std::atomic<AgentState *> g_state{nullptr};

int raw_open_read(const char *path) {
    return open(path, O_RDONLY | O_CLOEXEC);
}

bool read_file(const std::string &path, std::string *out, size_t limit = kMaxFile) {
    int fd = raw_open_read(path.c_str());
    if (fd < 0) return false;
    out->clear();
    char buffer[8192];
    while (out->size() < limit) {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        if (n == 0) break;
        out->append(buffer, static_cast<size_t>(n));
    }
    close(fd);
    return true;
}

std::string trim(const std::string &text) {
    size_t begin = 0, end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' ||
                           text[begin] == '\r' || text[begin] == '\n')) begin++;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                           text[end - 1] == '\r' || text[end - 1] == '\n')) end--;
    return text.substr(begin, end - begin);
}

std::string xml_unescape(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (text.compare(i, 6, "&quot;") == 0) { out.push_back('"'); i += 6; }
        else if (text.compare(i, 5, "&amp;") == 0) { out.push_back('&'); i += 5; }
        else if (text.compare(i, 4, "&lt;") == 0) { out.push_back('<'); i += 4; }
        else if (text.compare(i, 4, "&gt;") == 0) { out.push_back('>'); i += 4; }
        else if (text.compare(i, 5, "&#39;") == 0) { out.push_back('\''); i += 5; }
        else { out.push_back(text[i]); i++; }
    }
    return out;
}

std::string user_data_dir() {
    const uid_t uid = getuid();
    const unsigned int user_id = static_cast<unsigned int>(uid / 100000);
    return "/data/user/" + std::to_string(user_id) + "/com.zhiliaoapp.musically";
}

std::string prefs_dir() {
    return user_data_dir() + "/shared_prefs";
}

std::string xml_string_value(const std::string &xml, const std::string &name) {
    const std::string needle = "name=\"" + name + "\"";
    size_t position = xml.find(needle);
    if (position == std::string::npos) return "";
    position += needle.size();
    size_t tag_end = xml.find('>', position);
    if (tag_end == std::string::npos) return "";
    size_t value_end = xml.find("</string>", tag_end + 1);
    if (value_end == std::string::npos) return "";
    return xml_unescape(xml.substr(tag_end + 1, value_end - tag_end - 1));
}

std::string find_account_id(const std::string &xml) {
    const std::string suffix = "_account_user_info";
    size_t pos = xml.find(suffix);
    if (pos == std::string::npos) return "";
    size_t begin = pos;
    while (begin > 0 && xml[begin - 1] >= '0' && xml[begin - 1] <= '9') begin--;
    std::string id = xml.substr(begin, pos - begin);
    if (id.empty() || id.size() > 32) return "";
    return id;
}

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

bool hex_decode(const std::string &text, std::string *out) {
    if (text.size() % 2 != 0) return false;
    out->clear();
    out->reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        int high = hex_value(text[i]);
        int low = hex_value(text[i + 1]);
        if (high < 0 || low < 0) return false;
        out->push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

std::string parse_serialized_cookie(const std::string &hex, const std::string &name) {
    std::string bytes;
    if (!hex_decode(hex, &bytes)) return "";
    std::string needle = "t";
    needle.push_back(static_cast<char>((name.size() >> 8) & 0xff));
    needle.push_back(static_cast<char>(name.size() & 0xff));
    needle += name;
    size_t position = bytes.find(needle);
    if (position == std::string::npos) return "";
    position += needle.size();
    if (position + 3 > bytes.size() || bytes[position] != 't') return "";
    const size_t length = (static_cast<unsigned char>(bytes[position + 1]) << 8) |
                          static_cast<unsigned char>(bytes[position + 2]);
    position += 3;
    if (position + length > bytes.size()) return "";
    return bytes.substr(position, length);
}

cJSON *read_cookies() {
    std::string xml;
    if (!read_file(prefs_dir() + "/ttnetCookieStore.xml", &xml)) return nullptr;
    cJSON *array = cJSON_CreateArray();
    if (!array) return nullptr;
    size_t position = 0;
    while (true) {
        position = xml.find("<string name=\"", position);
        if (position == std::string::npos) break;
        position += 14;
        size_t key_end = xml.find("\">", position);
        if (key_end == std::string::npos) break;
        const std::string key = xml_unescape(xml.substr(position, key_end - position));
        const size_t value_start = key_end + 2;
        const size_t value_end = xml.find("</string>", value_start);
        if (value_end == std::string::npos) break;
        const std::string hex = xml.substr(value_start, value_end - value_start);
        position = value_end + 9;
        const size_t separator = key.find('|');
        if (separator == std::string::npos) continue;
        const std::string domain = key.substr(0, separator);
        const std::string name = key.substr(separator + 1);
        const std::string value = parse_serialized_cookie(hex, name);
        if (value.empty()) continue;
        cJSON *cookie = cJSON_CreateObject();
        if (!cookie) continue;
        cJSON_AddStringToObject(cookie, "domain", domain.c_str());
        cJSON_AddStringToObject(cookie, "name", name.c_str());
        cJSON_AddStringToObject(cookie, "value", value.c_str());
        cJSON_AddItemToArray(array, cookie);
    }
    return array;
}

std::string find_profile_username() {
    std::string xml;
    if (!read_file(prefs_dir() + "/aweme_user.xml", &xml)) return "";
    const std::string raw = xml_string_value(xml, "user_info_raw");
    if (raw.empty()) return "";
    cJSON *root = cJSON_Parse(raw.c_str());
    if (!root) return "";
    std::string username;
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON *name = data ? cJSON_GetObjectItemCaseSensitive(data, "username") : nullptr;
    if (cJSON_IsString(name)) username = name->valuestring;
    cJSON_Delete(root);
    return username;
}

std::string find_device_id() {
    for (const char *file : {"applog_stats.xml", "aweme_user.xml"}) {
        std::string xml;
        if (!read_file(prefs_dir() + "/" + file, &xml)) continue;
        const std::string value = trim(xml_string_value(xml, "device_id"));
        if (!value.empty()) return value;
    }
    return "";
}

std::string find_cookie_value(const std::string &wanted) {
    std::string xml;
    if (!read_file(prefs_dir() + "/ttnetCookieStore.xml", &xml)) return "";
    size_t position = 0;
    while (true) {
        position = xml.find("<string name=\"", position);
        if (position == std::string::npos) break;
        position += 14;
        size_t key_end = xml.find("\">", position);
        if (key_end == std::string::npos) break;
        const std::string key = xml_unescape(xml.substr(position, key_end - position));
        const size_t value_start = key_end + 2;
        const size_t value_end = xml.find("</string>", value_start);
        if (value_end == std::string::npos) break;
        position = value_end + 9;
        const size_t separator = key.find('|');
        if (separator == std::string::npos) continue;
        const std::string name = key.substr(separator + 1);
        if (name != wanted) continue;
        return parse_serialized_cookie(xml.substr(value_start, value_end - value_start), name);
    }
    return "";
}

std::string find_install_id() {
    return find_cookie_value("install_id");
}

bool detect_account(std::string *account, std::string *username, std::string *device_id,
                    std::string *install_id) {
    std::string xml;
    if (!read_file(prefs_dir() + "/aweme_user.xml", &xml)) return false;
    *account = find_account_id(xml);
    if (account->empty()) return false;
    *username = find_profile_username();
    *device_id = find_device_id();
    *install_id = find_install_id();
    return true;
}

/* ------------------------------------------------------------------------- */
/* router protocol                                                           */

bool sleep_interruptible(AgentState *state, int milliseconds) {
    for (int elapsed = 0; elapsed < milliseconds; elapsed += 50) {
        if (state->stop.load()) return false;
        usleep(50 * 1000);
    }
    return true;
}

void close_fd(AgentState *state) {
    const int fd = state->router_fd.exchange(-1);
    if (fd >= 0) close(fd);
}

bool send_all(int fd, const void *data, size_t size) {
    const char *cursor = static_cast<const char *>(data);
    while (size > 0) {
        ssize_t sent = send(fd, cursor, size, MSG_NOSIGNAL);
        if (sent <= 0) return false;
        cursor += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

bool send_json(int fd, cJSON *root) {
    char *text = cJSON_PrintUnformatted(root);
    if (!text) return false;
    bool ok = send_all(fd, text, strlen(text));
    if (ok) ok = send_all(fd, "\n", 1);
    cJSON_free(text);
    return ok;
}

char *read_line(AgentState *state, int fd, size_t *length) {
    std::string line;
    char buffer[4096];
    *length = 0;
    while (true) {
        if (state->stop.load()) return nullptr;
        ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
        if (count <= 0) {
            if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            return nullptr;
        }
        for (ssize_t i = 0; i < count; i++) {
            if (buffer[i] == '\n') {
                *length = line.size();
                char *result = static_cast<char *>(malloc(line.size() + 1));
                if (!result) return nullptr;
                memcpy(result, line.data(), line.size());
                result[line.size()] = '\0';
                return result;
            }
            line.push_back(buffer[i]);
            if (line.size() > kMaxLine) return nullptr;
        }
    }
}

void send_response(AgentState *state, int fd, const std::string &id, const std::string &route,
                   bool ok, cJSON *result_or_error) {
    (void)state;
    cJSON *root = cJSON_CreateObject();
    if (!root) { cJSON_Delete(result_or_error); return; }
    cJSON_AddStringToObject(root, "type", "response");
    cJSON_AddStringToObject(root, "id", id.c_str());
    cJSON_AddStringToObject(root, "route", route.c_str());
    cJSON_AddBoolToObject(root, "ok", ok ? 1 : 0);
    if (ok) cJSON_AddItemToObject(root, "result", result_or_error);
    else cJSON_AddItemToObject(root, "error", result_or_error);
    send_json(fd, root);
    cJSON_Delete(root);
}

void send_error(AgentState *state, int fd, const std::string &id, const std::string &route,
                const char *code, const char *message) {
    cJSON *error = cJSON_CreateObject();
    if (!error) return;
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    send_response(state, fd, id, route, false, error);
}

cJSON *credentials_result(const std::string &account) {
    std::string username, device_id, install_id, xml;
    read_file(prefs_dir() + "/aweme_user.xml", &xml);
    username = find_profile_username();
    device_id = find_device_id();
    install_id = find_install_id();
    cJSON *result = cJSON_CreateObject();
    if (!result) return nullptr;
    cJSON_AddStringToObject(result, "account_id", account.c_str());
    cJSON_AddStringToObject(result, "username", username.c_str());
    cJSON_AddStringToObject(result, "device_id", device_id.c_str());
    cJSON_AddStringToObject(result, "install_id", install_id.c_str());
    cJSON_AddNumberToObject(result, "android_user", static_cast<double>(getuid() / 100000));
    cJSON_AddNumberToObject(result, "updated_at", static_cast<double>(time(nullptr)));
    cJSON *cookies = read_cookies();
    if (cookies) cJSON_AddItemToObject(result, "cookies", cookies);
    else cJSON_AddItemToObject(result, "cookies", cJSON_CreateArray());
    return result;
}

void handle_request(AgentState *state, int fd, const char *line, size_t length,
                    const std::string &account) {
    cJSON *root = cJSON_ParseWithLength(line, length);
    if (!root) return;
    cJSON *id_node = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *route_node = cJSON_GetObjectItemCaseSensitive(root, "route");
    cJSON *method_node = cJSON_GetObjectItemCaseSensitive(root, "method");
    std::string id = cJSON_IsString(id_node) ? id_node->valuestring : "";
    std::string route = cJSON_IsString(route_node) ? route_node->valuestring : "";
    std::string method = cJSON_IsString(method_node) ? method_node->valuestring : "";
    if (id.empty()) { cJSON_Delete(root); send_error(state, fd, "", route, "invalid_request", "id is required"); return; }
    if (route != account) { cJSON_Delete(root); send_error(state, fd, id, route, "route_mismatch", "route does not match current account"); return; }
    if (method == "ping") {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "plugin", "tiktok-agent");
        cJSON_AddStringToObject(result, "account_id", account.c_str());
        cJSON_AddStringToObject(result, "version", kVersion);
        send_response(state, fd, id, route, true, result);
    } else if (method == "credentials" || method == "refresh") {
        cJSON *result = credentials_result(account);
        if (!result) send_error(state, fd, id, route, "credentials_unavailable", "credential files are unavailable");
        else send_response(state, fd, id, route, true, result);
    } else {
        send_error(state, fd, id, route, "method_not_found", "unknown method");
    }
    cJSON_Delete(root);
}

void serve_router(AgentState *state, int fd, const std::string &account) {
    struct timeval timeout = { .tv_sec = 0, .tv_usec = 500000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    cJSON *hello = cJSON_CreateObject();
    cJSON_AddStringToObject(hello, "type", "hello");
    cJSON_AddStringToObject(hello, "app", "com.zhiliaoapp.musically");
    cJSON_AddStringToObject(hello, "plugin", "tiktok_agent");
    cJSON_AddStringToObject(hello, "version", kVersion);
    cJSON *routes = cJSON_AddArrayToObject(hello, "routes");
    cJSON_AddItemToArray(routes, cJSON_CreateString(account.c_str()));
    cJSON *accounts = cJSON_AddArrayToObject(hello, "accounts");
    cJSON_AddItemToArray(accounts, cJSON_CreateString(account.c_str()));
    bool sent = send_json(fd, hello);
    cJSON_Delete(hello);
    if (!sent) return;
    LOGI("connected to dejavurouter route=%s", account.c_str());
    while (!state->stop.load()) {
        size_t length = 0;
        char *line = read_line(state, fd, &length);
        if (!line) break;
        handle_request(state, fd, line, length, account);
        free(line);
    }
}

int connect_router() {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(kRouterPort);
    if (inet_pton(AF_INET, kRouterHost, &address.sin_addr) != 1) { close(fd); return -1; }
    if (connect(fd, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) != 0) { close(fd); return -1; }
    return fd;
}

void *agent_main(void *arg) {
    AgentState *state = static_cast<AgentState *>(arg);
    LOGI("worker started pid=%d uid=%u", getpid(), static_cast<unsigned>(getuid()));
    while (!state->stop.load()) {
        std::string account, username, device_id, install_id;
        if (!detect_account(&account, &username, &device_id, &install_id)) {
            if (!sleep_interruptible(state, 2000)) break;
            continue;
        }
        int fd = connect_router();
        if (fd < 0) { if (!sleep_interruptible(state, 1000)) break; continue; }
        state->router_fd.store(fd);
        serve_router(state, fd, account);
        close_fd(state);
        if (!sleep_interruptible(state, 1000)) break;
    }
    LOGI("worker stopped");
    return nullptr;
}

int plugin_initialize(const DejavuPluginHost *host, void **plugin_state, char *error,
                      size_t error_size) {
    if (!host || !plugin_state || host->abi_version != DEJAVU_PLUGIN_ABI_VERSION) {
        snprintf(error, error_size, "incompatible plugin host");
        return -1;
    }
    auto *state = new (std::nothrow) AgentState();
    if (!state) { snprintf(error, error_size, "out of memory"); return -1; }
    state->host = host;
    g_state.store(state);
    if (pthread_create(&state->thread, nullptr, agent_main, state) != 0) {
        g_state.store(nullptr);
        delete state;
        snprintf(error, error_size, "cannot start worker thread");
        return -1;
    }
    state->thread_started = true;
    LOGI("plugin initialized");
    return 0;
}

void plugin_stop(AgentState *state) {
    if (!state) return;
    state->stop.store(true);
    close_fd(state);
    if (state->thread_started) { pthread_join(state->thread, nullptr); state->thread_started = false; }
}

void plugin_control_ready(void *plugin_state) {
    auto *state = static_cast<AgentState *>(plugin_state);
    if (state && state->host && state->host->log_message) {
        state->host->log_message(state->host->host_context, ANDROID_LOG_INFO,
                                 "tiktok-agent ready: route is the TikTok user id");
    }
}

void plugin_unload_prepare(void *plugin_state) { plugin_stop(static_cast<AgentState *>(plugin_state)); }

void plugin_shutdown(void *plugin_state) {
    auto *state = static_cast<AgentState *>(plugin_state);
    if (!state) return;
    plugin_stop(state);
    g_state.store(nullptr);
    delete state;
    LOGI("plugin shutdown");
}

} // namespace

extern "C" __attribute__((visibility("default"))) const DejavuPluginDescriptor *
dejavu_plugin_entry(void) {
    static DejavuPluginDescriptor descriptor = {
        DEJAVU_PLUGIN_ABI_VERSION,
        sizeof(DejavuPluginDescriptor),
        "tiktok_agent",
        "0.1.0",
        DEJAVU_PLUGIN_FLAG_UNLOAD_SAFE,
        plugin_initialize,
        plugin_control_ready,
        plugin_shutdown,
        0,
        plugin_unload_prepare,
    };
    return &descriptor;
}