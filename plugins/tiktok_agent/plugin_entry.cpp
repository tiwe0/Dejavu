#include "dejavu_plugin.h"
#include "cJSON.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <jni.h>
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
    JavaVM *vm = nullptr;
    pthread_t thread{};
    bool thread_started = false;
    std::atomic<bool> stop{false};
    std::atomic<int> router_fd{-1};
    pthread_mutex_t signer_mutex = PTHREAD_MUTEX_INITIALIZER;
    jobject class_loader = nullptr;
    jclass rpc_class = nullptr;
    jmethodID sign_method = nullptr;
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

std::string jstring_utf(JNIEnv *env, jstring value) {
    if (!value) return "";
    const char *chars = env->GetStringUTFChars(value, nullptr);
    std::string out = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(value, chars);
    return out;
}

bool ensure_signer(JNIEnv *env, AgentState *state, std::string *error) {
    pthread_mutex_lock(&state->signer_mutex);
    if (state->rpc_class != nullptr && state->sign_method != nullptr) {
        pthread_mutex_unlock(&state->signer_mutex);
        return true;
    }
    bool ok = false;
    std::string dex;
    const std::string path = user_data_dir() + "/files/ttrpc.dex";
    if (!read_file(path, &dex)) {
        *error = "ttrpc dex not readable";
        pthread_mutex_unlock(&state->signer_mutex);
        return false;
    }
    if (env->PushLocalFrame(32) != 0) {
        *error = "JNI local frame failed";
        pthread_mutex_unlock(&state->signer_mutex);
        return false;
    }
    jclass activity_thread = env->FindClass("android/app/ActivityThread");
    jmethodID current_application = activity_thread
        ? env->GetStaticMethodID(activity_thread, "currentApplication",
                                 "()Landroid/app/Application;") : nullptr;
    jobject application = current_application
        ? env->CallStaticObjectMethod(activity_thread, current_application) : nullptr;
    jclass context_class = env->FindClass("android/content/Context");
    jmethodID get_class_loader = context_class
        ? env->GetMethodID(context_class, "getClassLoader", "()Ljava/lang/ClassLoader;") : nullptr;
    jobject parent = (application && get_class_loader)
        ? env->CallObjectMethod(application, get_class_loader) : nullptr;
    jobject buffer = env->NewDirectByteBuffer(const_cast<char *>(dex.data()),
                                               static_cast<jlong>(dex.size()));
    jclass loader_class = env->FindClass("dalvik/system/InMemoryDexClassLoader");
    jmethodID loader_ctor = loader_class
        ? env->GetMethodID(loader_class, "<init>",
                           "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V") : nullptr;
    jobject loader = (buffer && loader_ctor && loader_class)
        ? env->NewObject(loader_class, loader_ctor, buffer, parent) : nullptr;
    jclass loader_parent = loader ? env->FindClass("java/lang/ClassLoader") : nullptr;
    jmethodID load_class = loader_parent
        ? env->GetMethodID(loader_parent, "loadClass",
                           "(Ljava/lang/String;)Ljava/lang/Class;") : nullptr;
    jstring rpc_name = env->NewStringUTF("ttrpc.Rpc");
    jclass rpc = (loader && load_class)
        ? static_cast<jclass>(env->CallObjectMethod(loader, load_class, rpc_name)) : nullptr;
    jmethodID sign = rpc
        ? env->GetStaticMethodID(rpc, "sign",
                                 "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;") : nullptr;
    if (sign && !env->ExceptionCheck()) {
        state->class_loader = env->NewGlobalRef(loader);
        state->rpc_class = static_cast<jclass>(env->NewGlobalRef(rpc));
        state->sign_method = sign;
        ok = true;
    } else {
        env->ExceptionClear();
        *error = "ttrpc.Rpc#sign unavailable";
    }
    env->PopLocalFrame(nullptr);
    pthread_mutex_unlock(&state->signer_mutex);
    return ok;
}

bool sign_url(JNIEnv *env, AgentState *state, const std::string &url,
              const std::string &headers_json, cJSON **out, std::string *error) {
    if (!ensure_signer(env, state, error)) return false;
    if (env->PushLocalFrame(16) != 0) {
        *error = "JNI local frame failed";
        return false;
    }
    jstring jurl = env->NewStringUTF(url.c_str());
    jstring jheaders = env->NewStringUTF(headers_json.c_str());
    jstring result = static_cast<jstring>(
        env->CallStaticObjectMethod(state->rpc_class, state->sign_method, jurl, jheaders));
    if (env->ExceptionCheck() || !result) {
        env->ExceptionClear();
        *error = "ttrpc.Rpc.sign failed";
        env->PopLocalFrame(nullptr);
        return false;
    }
    const std::string text = jstring_utf(env, result);
    cJSON *root = cJSON_Parse(text.c_str());
    if (!root) {
        *error = "ttrpc sign returned invalid JSON";
        env->PopLocalFrame(nullptr);
        return false;
    }
    cJSON *direct = cJSON_GetObjectItemCaseSensitive(root, "direct");
    cJSON *headers = direct ? cJSON_GetObjectItemCaseSensitive(direct, "headers") : nullptr;
    if (!cJSON_IsObject(headers)) {
        cJSON *captured = cJSON_GetObjectItemCaseSensitive(root, "captured");
        cJSON *best = captured ? cJSON_GetObjectItemCaseSensitive(captured, "best") : nullptr;
        cJSON *sent = best ? cJSON_GetObjectItemCaseSensitive(best, "sent") : nullptr;
        headers = sent ? cJSON_GetObjectItemCaseSensitive(sent, "headers") : nullptr;
    }
    if (!cJSON_IsObject(headers)) {
        cJSON_Delete(root);
        *error = "ttrpc sign returned no headers";
        env->PopLocalFrame(nullptr);
        return false;
    }
    *out = cJSON_Duplicate(headers, 1);
    cJSON_Delete(root);
    if (!*out) {
        *error = "unable to copy sign headers";
        env->PopLocalFrame(nullptr);
        return false;
    }
    env->PopLocalFrame(nullptr);
    return true;
}

void handle_request(AgentState *state, JNIEnv *env, int fd, const char *line, size_t length,
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
    } else if (method == "sign") {
        cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
        cJSON *url_node = params ? cJSON_GetObjectItemCaseSensitive(params, "url") : nullptr;
        cJSON *headers_node = params ? cJSON_GetObjectItemCaseSensitive(params, "headers") : nullptr;
        if (!cJSON_IsString(url_node) || !url_node->valuestring || !url_node->valuestring[0]) {
            send_error(state, fd, id, route, "invalid_request", "url is required");
        } else {
            char *headers_text = cJSON_IsObject(headers_node)
                ? cJSON_PrintUnformatted(headers_node) : strdup("{}");
            std::string error;
            cJSON *headers = nullptr;
            if (!headers_text || !sign_url(env, state, url_node->valuestring,
                                           headers_text ? headers_text : "{}", &headers, &error)) {
                if (headers_text) cJSON_free(headers_text);
                send_error(state, fd, id, route, "signer_unavailable",
                           error.empty() ? "ttrpc signer unavailable" : error.c_str());
            } else {
                cJSON_free(headers_text);
                cJSON *result = cJSON_CreateObject();
                cJSON_AddStringToObject(result, "account_id", account.c_str());
                cJSON_AddItemToObject(result, "headers", headers);
                send_response(state, fd, id, route, true, result);
            }
        }
    } else {
        send_error(state, fd, id, route, "method_not_found", "unknown method");
    }
    cJSON_Delete(root);
}

void serve_router(AgentState *state, JNIEnv *env, int fd, const std::string &account) {
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
        handle_request(state, env, fd, line, length, account);
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
    JNIEnv *env = nullptr;
    if (state->vm) state->vm->AttachCurrentThread(&env, nullptr);
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
        serve_router(state, env, fd, account);
        close_fd(state);
        if (!sleep_interruptible(state, 1000)) break;
    }
    if (env && state->vm) state->vm->DetachCurrentThread();
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
    state->vm = host->java_vm;
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