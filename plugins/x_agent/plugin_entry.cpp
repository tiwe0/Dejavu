#include "dejavu_plugin.h"
#include "cJSON.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <jni.h>
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

#define LOG_TAG "XAgentPlugin"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr const char *kVersion = "0.1.0";
constexpr const char *kRouterHost = "127.0.0.1";
constexpr int kRouterPort = 39111;
constexpr const char *kAliasPrefix = "twitter-android-keystore-alias-";
constexpr const char *kTokenFiles[] = {
    "hardware_attestation_token_cache.xml",
    "app_attest_token_cache.xml",
};
constexpr size_t kMaxLine = 1024 * 1024;
constexpr size_t kMaxFile = 2 * 1024 * 1024;

struct AgentState {
    const DejavuPluginHost *host = nullptr;
    pthread_t thread{};
    bool thread_started = false;
    std::atomic<bool> stop{false};
    std::atomic<int> router_fd{-1};
};

std::atomic<AgentState *> g_state{nullptr};

/* ------------------------------------------------------------------------- */
/* basic IO                                                                  */

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

bool sleep_interruptible(AgentState *state, int milliseconds) {
    int slept = 0;
    while (!state->stop.load() && slept < milliseconds) {
        usleep(100 * 1000);
        slept += 100;
    }
    return !state->stop.load();
}

void close_fd(AgentState *state) {
    int fd = state->router_fd.exchange(-1);
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
}

/* ------------------------------------------------------------------------- */
/* base64 and JWT claims                                                     */

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const unsigned char *data, size_t size) {
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= size) {
        unsigned int v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);
        out.push_back(kB64[v & 63]);
        i += 3;
    }
    if (i + 1 == size) {
        unsigned int v = data[i] << 16;
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out += "==";
    } else if (i + 2 == size) {
        unsigned int v = (data[i] << 16) | (data[i + 1] << 8);
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

bool base64_decode(const std::string &text, std::vector<unsigned char> *out) {
    out->clear();
    int buffer = 0, bits = 0;
    for (char ch : text) {
        if (ch == '=' || ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t') continue;
        int value = b64_value(ch);
        if (value < 0) return false;
        buffer = (buffer << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(static_cast<unsigned char>((buffer >> bits) & 0xff));
        }
        if (out->size() > kMaxLine) return false;
    }
    return true;
}

std::string iso_utc(time_t seconds) {
    struct tm tm;
    gmtime_r(&seconds, &tm);
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

bool token_expiry(const std::string &token, std::string *account_out, std::string *expires_out,
                  long long *expires_in_out) {
    size_t first = token.find('.');
    if (first == std::string::npos) return false;
    size_t second = token.find('.', first + 1);
    if (second == std::string::npos) return false;
    std::string payload = token.substr(first + 1, second - first - 1);
    std::vector<unsigned char> raw;
    if (!base64_decode(payload, &raw)) return false;
    cJSON *root = cJSON_ParseWithLength(reinterpret_cast<const char *>(raw.data()), raw.size());
    if (!root) return false;
    cJSON *account = cJSON_GetObjectItemCaseSensitive(root, "accountId");
    cJSON *expiry = cJSON_GetObjectItemCaseSensitive(root, "expiresAt");
    bool ok = false;
    if (cJSON_IsString(account) && cJSON_IsString(expiry)) {
        char *end = nullptr;
        long long value = strtoll(expiry->valuestring, &end, 10);
        if (end && *end == 0) {
            if (value > 1000000000000LL) value /= 1000;
            *account_out = account->valuestring;
            *expires_out = iso_utc(static_cast<time_t>(value));
            *expires_in_out = value - static_cast<long long>(time(nullptr));
            ok = true;
        }
    }
    cJSON_Delete(root);
    return ok;
}

/* ------------------------------------------------------------------------- */
/* preferences: account id and cached token                                  */

std::string user_prefs_dir() {
    uid_t uid = getuid();
    unsigned int user_id = static_cast<unsigned int>(uid / 100000);
    return "/data/user/" + std::to_string(user_id) + "/com.twitter.android/shared_prefs";
}

std::string xml_value_after(const std::string &xml, const std::string &needle) {
    size_t position = xml.find(needle);
    if (position == std::string::npos) return "";
    position += needle.size();
    size_t value_start = xml.find("value=\"", position);
    if (value_start != std::string::npos) {
        value_start += 7;
        size_t value_end = xml.find('"', value_start);
        if (value_end != std::string::npos) return xml.substr(value_start, value_end - value_start);
    }
    size_t tag_end = xml.find('>', position);
    if (tag_end != std::string::npos) {
        size_t value_end = xml.find('<', tag_end + 1);
        if (value_end != std::string::npos) return trim(xml.substr(tag_end + 1, value_end - tag_end - 1));
    }
    return "";
}

std::string detect_account() {
    std::string xml;
    if (!read_file(user_prefs_dir() + "/com.twitter.android_preferences.xml", &xml)) return "";
    std::string account = xml_value_after(xml, "name=\"current_user_id\"");
    account = trim(account);
    if (account.empty() || account.size() > 64) return "";
    for (char ch : account) {
        if (ch < '0' || ch > '9') return "";
    }
    return account;
}

std::string read_cached_token(const std::string &account) {
    for (const char *file : kTokenFiles) {
        std::string xml;
        if (!read_file(user_prefs_dir() + "/" + file, &xml)) continue;
        std::string needle = "name=\"" + account + "\">";
        size_t position = xml.find(needle);
        if (position == std::string::npos) continue;
        position += needle.size();
        size_t end = xml.find('<', position);
        if (end == std::string::npos) continue;
        std::string token;
        token.reserve(end - position);
        for (size_t i = position; i < end; i++) {
            char ch = xml[i];
            if (ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t') token.push_back(ch);
        }
        if (!token.empty()) return token;
    }
    return "";
}

/* ------------------------------------------------------------------------- */
/* AndroidKeyStore signing                                                   */

std::string jstring_to_utf(JNIEnv *env, jstring value) {
    if (!value) return "";
    const char *chars = env->GetStringUTFChars(value, nullptr);
    std::string out = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(value, chars);
    return out;
}

std::string take_exception(JNIEnv *env) {
    if (!env->ExceptionCheck()) return "";
    jthrowable throwable = env->ExceptionOccurred();
    env->ExceptionClear();
    jclass throwable_class = env->FindClass("java/lang/Throwable");
    if (!throwable_class) { env->ExceptionClear(); return "java exception"; }
    jmethodID to_string = env->GetMethodID(throwable_class, "toString", "()Ljava/lang/String;");
    std::string message = "java exception";
    if (to_string) {
        auto text = static_cast<jstring>(env->CallObjectMethod(throwable, to_string));
        if (!env->ExceptionCheck()) message = jstring_to_utf(env, text);
        else env->ExceptionClear();
        if (text) env->DeleteLocalRef(text);
    }
    env->DeleteLocalRef(throwable);
    env->DeleteLocalRef(throwable_class);
    return message;
}

bool sign_digest(JNIEnv *env, const std::string &account, const std::vector<unsigned char> &digest,
                 std::string *der_b64, std::string *error) {
    if (!env) { *error = "no JNIEnv"; return false; }
    if (env->PushLocalFrame(32) != 0) { *error = "JNI local frame failed"; return false; }
    const std::string alias = std::string(kAliasPrefix) + account;
    bool ok = false;
    jclass key_store_class = env->FindClass("java/security/KeyStore");
    jmethodID get_instance = key_store_class ? env->GetStaticMethodID(
        key_store_class, "getInstance", "(Ljava/lang/String;)Ljava/security/KeyStore;") : nullptr;
    jmethodID load = key_store_class ? env->GetMethodID(
        key_store_class, "load", "(Ljava/security/KeyStore$LoadStoreParameter;)V") : nullptr;
    jmethodID get_key = key_store_class ? env->GetMethodID(
        key_store_class, "getKey", "(Ljava/lang/String;[C)Ljava/security/Key;") : nullptr;
    if (!get_instance || !load || !get_key) {
        *error = take_exception(env);
        if (error->empty()) *error = "keystore methods unavailable";
        env->PopLocalFrame(nullptr);
        return false;
    }
    jstring provider = env->NewStringUTF("AndroidKeyStore");
    auto store = static_cast<jobject>(env->CallStaticObjectMethod(key_store_class, get_instance, provider));
    if (env->ExceptionCheck() || !store) {
        *error = take_exception(env);
        if (error->empty()) *error = "AndroidKeyStore unavailable";
        env->PopLocalFrame(nullptr);
        return false;
    }
    env->CallVoidMethod(store, load, static_cast<jobject>(nullptr));
    if (env->ExceptionCheck()) {
        *error = take_exception(env);
        env->PopLocalFrame(nullptr);
        return false;
    }
    jstring alias_string = env->NewStringUTF(alias.c_str());
    auto key = static_cast<jobject>(env->CallObjectMethod(store, get_key, alias_string,
                                                          static_cast<jcharArray>(nullptr)));
    if (env->ExceptionCheck()) {
        *error = take_exception(env);
        env->PopLocalFrame(nullptr);
        return false;
    }
    if (!key) {
        *error = "keystore key not found for alias " + alias;
        env->PopLocalFrame(nullptr);
        return false;
    }
    jclass signature_class = env->FindClass("java/security/Signature");
    jmethodID get_signature = signature_class ? env->GetStaticMethodID(
        signature_class, "getInstance", "(Ljava/lang/String;)Ljava/security/Signature;") : nullptr;
    jmethodID init_sign = signature_class ? env->GetMethodID(
        signature_class, "initSign", "(Ljava/security/PrivateKey;)V") : nullptr;
    jmethodID update = signature_class ? env->GetMethodID(signature_class, "update", "([B)V") : nullptr;
    jmethodID sign = signature_class ? env->GetMethodID(signature_class, "sign", "()[B") : nullptr;
    if (!get_signature || !init_sign || !update || !sign) {
        *error = take_exception(env);
        if (error->empty()) *error = "signature methods unavailable";
        env->PopLocalFrame(nullptr);
        return false;
    }
    jstring algorithm = env->NewStringUTF("SHA256withECDSA");
    auto signer = static_cast<jobject>(env->CallStaticObjectMethod(signature_class, get_signature, algorithm));
    if (env->ExceptionCheck() || !signer) {
        *error = take_exception(env);
        if (error->empty()) *error = "Signature.getInstance failed";
        env->PopLocalFrame(nullptr);
        return false;
    }
    env->CallVoidMethod(signer, init_sign, key);
    if (!env->ExceptionCheck()) {
        jbyteArray input = env->NewByteArray(static_cast<jsize>(digest.size()));
        env->SetByteArrayRegion(input, 0, static_cast<jsize>(digest.size()),
                                reinterpret_cast<const jbyte *>(digest.data()));
        env->CallVoidMethod(signer, update, input);
        if (!env->ExceptionCheck()) {
            auto output = static_cast<jbyteArray>(env->CallObjectMethod(signer, sign));
            if (!env->ExceptionCheck() && output) {
                jsize size = env->GetArrayLength(output);
                std::vector<unsigned char> der(static_cast<size_t>(size));
                env->GetByteArrayRegion(output, 0, size, reinterpret_cast<jbyte *>(der.data()));
                *der_b64 = base64_encode(der.data(), der.size());
                ok = true;
            } else {
                *error = take_exception(env);
                if (error->empty()) *error = "signature output is null";
            }
        } else {
            *error = take_exception(env);
        }
    } else {
        *error = take_exception(env);
    }
    env->PopLocalFrame(nullptr);
    return ok;
}

/* ------------------------------------------------------------------------- */
/* router TCP client                                                         */

bool send_all(int fd, const void *data, size_t size) {
    const char *bytes = static_cast<const char *>(data);
    size_t offset = 0;
    while (offset < size) {
        ssize_t n = send(fd, bytes + offset, size - offset, MSG_NOSIGNAL);
        if (n <= 0) return false;
        offset += static_cast<size_t>(n);
    }
    return true;
}

bool send_json(int fd, cJSON *root) {
    char *text = cJSON_PrintUnformatted(root);
    if (!text) return false;
    bool ok = send_all(fd, text, strlen(text)) && send_all(fd, "\n", 1);
    free(text);
    return ok;
}

char *read_line(AgentState *state, int fd, size_t *length) {
    std::string line;
    char ch;
    while (!state->stop.load()) {
        ssize_t n = recv(fd, &ch, 1, 0);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
            return nullptr;
        }
        if (ch == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            *length = line.size();
            char *out = static_cast<char *>(malloc(line.size() + 1));
            if (!out) return nullptr;
            memcpy(out, line.data(), line.size());
            out[line.size()] = 0;
            return out;
        }
        if (line.size() >= kMaxLine) return nullptr;
        line.push_back(ch);
    }
    return nullptr;
}

cJSON *error_object(const char *code, const char *message) {
    cJSON *error = cJSON_CreateObject();
    if (!error) return nullptr;
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddBoolToObject(error, "retryable", 1);
    return error;
}

void send_response(AgentState *state, int fd, const std::string &id, const std::string &route,
                   bool ok, cJSON *payload) {
    cJSON *root = cJSON_CreateObject();
    if (!root) { if (!ok) cJSON_Delete(payload); return; }
    cJSON_AddStringToObject(root, "type", "response");
    cJSON_AddStringToObject(root, "id", id.c_str());
    cJSON_AddStringToObject(root, "route", route.c_str());
    cJSON_AddBoolToObject(root, "ok", ok);
    if (ok) {
        if (payload) cJSON_AddItemReferenceToObject(root, "result", payload);
        else cJSON_AddObjectToObject(root, "result");
    } else {
        if (payload) cJSON_AddItemReferenceToObject(root, "error", payload);
    }
    if (!send_json(fd, root)) {
        LOGI("router write failed");
        close_fd(state);
    }
    cJSON_Delete(root);
    if (!ok && payload) cJSON_Delete(payload);
}

void send_plugin_error(AgentState *state, int fd, const std::string &id, const std::string &route,
                       const char *code, const char *message) {
    cJSON *error = error_object(code, message);
    send_response(state, fd, id, route, false, error);
}

bool bring_x_to_front(JNIEnv *env, std::string *error) {
    if (!env) { *error = "no JNIEnv"; return false; }
    if (env->PushLocalFrame(32) != 0) { *error = "JNI local frame failed"; return false; }
    bool ok = false;
    jclass activity_thread = env->FindClass("android/app/ActivityThread");
    jmethodID current_application = activity_thread
        ? env->GetStaticMethodID(activity_thread, "currentApplication",
                                 "()Landroid/app/Application;") : nullptr;
    jobject application = (activity_thread && current_application)
        ? static_cast<jobject>(env->CallStaticObjectMethod(activity_thread, current_application))
        : nullptr;
    if (!application || env->ExceptionCheck()) {
        *error = take_exception(env);
        if (error->empty()) *error = "no application context";
        env->PopLocalFrame(nullptr);
        return false;
    }
    jclass context_class = env->FindClass("android/content/Context");
    jmethodID get_package_manager = context_class
        ? env->GetMethodID(context_class, "getPackageManager",
                           "()Landroid/content/pm/PackageManager;") : nullptr;
    jclass package_manager_class = env->FindClass("android/content/pm/PackageManager");
    jmethodID get_launch_intent = package_manager_class
        ? env->GetMethodID(package_manager_class, "getLaunchIntentForPackage",
                           "(Ljava/lang/String;)Landroid/content/Intent;") : nullptr;
    if (!get_package_manager || !get_launch_intent) {
        *error = take_exception(env);
        if (error->empty()) *error = "package manager unavailable";
        env->PopLocalFrame(nullptr);
        return false;
    }
    jobject package_manager = env->CallObjectMethod(application, get_package_manager);
    jstring package_name = env->NewStringUTF("com.twitter.android");
    jobject intent = (package_manager && !env->ExceptionCheck())
        ? static_cast<jobject>(env->CallObjectMethod(package_manager, get_launch_intent, package_name))
        : nullptr;
    if (!intent || env->ExceptionCheck()) {
        *error = take_exception(env);
        if (error->empty()) *error = "no launch intent";
        env->PopLocalFrame(nullptr);
        return false;
    }
    jclass intent_class = env->FindClass("android/content/Intent");
    jmethodID add_flags = intent_class
        ? env->GetMethodID(intent_class, "addFlags", "(I)Landroid/content/Intent;") : nullptr;
    jmethodID start_activity = context_class
        ? env->GetMethodID(context_class, "startActivity", "(Landroid/content/Intent;)V") : nullptr;
    if (!add_flags || !start_activity) {
        *error = take_exception(env);
        if (error->empty()) *error = "activity methods unavailable";
        env->PopLocalFrame(nullptr);
        return false;
    }
    env->CallObjectMethod(intent, add_flags, 0x10000000 | 0x20000000);
    env->CallVoidMethod(application, start_activity, intent);
    if (env->ExceptionCheck()) {
        *error = take_exception(env);
        if (error->empty()) *error = "startActivity failed";
    } else {
        ok = true;
    }
    env->PopLocalFrame(nullptr);
    return ok;
}

void handle_request(AgentState *state, JNIEnv *env, int fd, const char *line, size_t length,
                    const std::string &account) {
    cJSON *root = cJSON_ParseWithLength(line, length);
    if (!root) return;
    cJSON *id_node = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *route_node = cJSON_GetObjectItemCaseSensitive(root, "route");
    cJSON *method_node = cJSON_GetObjectItemCaseSensitive(root, "method");
    cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    std::string id = cJSON_IsString(id_node) ? id_node->valuestring : "";
    std::string route = cJSON_IsString(route_node) ? route_node->valuestring : "";
    std::string method = cJSON_IsString(method_node) ? method_node->valuestring : "";
    if (id.empty()) {
        cJSON_Delete(root);
        send_plugin_error(state, fd, "", route, "invalid_request", "id is required");
        return;
    }
    if (route != account) {
        cJSON_Delete(root);
        send_plugin_error(state, fd, id, route, "route_mismatch", "route does not match current account");
        return;
    }
    if (method == "ping") {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "plugin", "x-agent");
        cJSON_AddStringToObject(result, "account_id", account.c_str());
        cJSON_AddStringToObject(result, "version", kVersion);
        send_response(state, fd, id, route, true, result);
        cJSON_Delete(result);
    } else if (method == "token") {
        std::string token = read_cached_token(account);
        if (token.empty()) {
            send_plugin_error(state, fd, id, route, "token_unavailable", "attestation token not in cache");
        } else {
            std::string claim_account, expires_at;
            long long expires_in = 0;
            bool claims_ok = token_expiry(token, &claim_account, &expires_at, &expires_in);
            if (claims_ok && claim_account != account) {
                send_plugin_error(state, fd, id, route, "token_mismatch", "cached token belongs to another account");
            } else {
                cJSON *result = cJSON_CreateObject();
                cJSON_AddStringToObject(result, "account_id", account.c_str());
                cJSON_AddStringToObject(result, "token", token.c_str());
                if (claims_ok) {
                    cJSON_AddStringToObject(result, "expires_at", expires_at.c_str());
                    cJSON_AddNumberToObject(result, "expires_in", static_cast<double>(expires_in));
                }
                send_response(state, fd, id, route, true, result);
                cJSON_Delete(result);
            }
        }
    } else if (method == "sign") {
        cJSON *digest_node = params ? cJSON_GetObjectItemCaseSensitive(params, "digest") : nullptr;
        std::vector<unsigned char> digest;
        if (!cJSON_IsString(digest_node) || !base64_decode(digest_node->valuestring, &digest) ||
            digest.size() != 32) {
            send_plugin_error(state, fd, id, route, "invalid_digest", "digest must be base64 SHA-256");
        } else {
            std::string signature, error;
            if (!sign_digest(env, account, digest, &signature, &error)) {
                send_plugin_error(state, fd, id, route, "sign_failed", error.empty() ? "sign failed" : error.c_str());
            } else {
                cJSON *result = cJSON_CreateObject();
                cJSON_AddStringToObject(result, "account_id", account.c_str());
                cJSON_AddStringToObject(result, "signature", signature.c_str());
                send_response(state, fd, id, route, true, result);
                cJSON_Delete(result);
            }
        }
    } else if (method == "refresh") {
        long long timeout_ms = 15000;
        cJSON *timeout_node = params ? cJSON_GetObjectItemCaseSensitive(params, "timeout_ms") : nullptr;
        if (cJSON_IsNumber(timeout_node)) timeout_ms = timeout_node->valueint;
        if (timeout_ms < 1000) timeout_ms = 1000;
        if (timeout_ms > 30000) timeout_ms = 30000;
        std::string old_token = read_cached_token(account);
        std::string old_claim, old_expires_at;
        long long old_expires_in = 0;
        if (!old_token.empty()) token_expiry(old_token, &old_claim, &old_expires_at, &old_expires_in);
        std::string trigger_error;
        bool triggered = bring_x_to_front(env, &trigger_error);
        bool observed = false;
        std::string new_token, new_claim, new_expires_at;
        long long new_expires_in = 0;
        long long waited = 0;
        while (waited < timeout_ms) {
            usleep(1000 * 1000);
            waited += 1000;
            new_token = read_cached_token(account);
            if (new_token.empty()) continue;
            new_expires_in = 0;
            bool claims = token_expiry(new_token, &new_claim, &new_expires_at, &new_expires_in);
            if (!claims || new_claim != account) continue;
            if (old_expires_in <= 0 || new_expires_in > old_expires_in + 2) {
                observed = true;
                break;
            }
        }
        if (!triggered) {
            send_plugin_error(state, fd, id, route, "refresh_trigger_failed",
                              trigger_error.empty() ? "could not foreground X app" : trigger_error.c_str());
        } else if (!observed) {
            cJSON *error = error_object("refresh_not_observed",
                                        "token cache did not refresh before the deadline");
            if (error) {
                cJSON_AddNumberToObject(error, "previous_expires_in", static_cast<double>(old_expires_in));
                send_response(state, fd, id, route, false, error);
            }
        } else {
            cJSON *result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "account_id", account.c_str());
            cJSON_AddStringToObject(result, "token", new_token.c_str());
            cJSON_AddStringToObject(result, "expires_at", new_expires_at.c_str());
            cJSON_AddNumberToObject(result, "expires_in", static_cast<double>(new_expires_in));
            cJSON_AddNumberToObject(result, "previous_expires_in", static_cast<double>(old_expires_in));
            cJSON_AddBoolToObject(result, "refreshed", 1);
            cJSON_AddBoolToObject(result, "app_foregrounded", triggered);
            send_response(state, fd, id, route, true, result);
            cJSON_Delete(result);
        }
    } else {
        send_plugin_error(state, fd, id, route, "method_not_found", "unknown method");
    }
    cJSON_Delete(root);
}

void serve_router(AgentState *state, JNIEnv *env, int fd, const std::string &account) {
    struct timeval timeout = { .tv_sec = 0, .tv_usec = 500000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    cJSON *hello = cJSON_CreateObject();
    cJSON_AddStringToObject(hello, "type", "hello");
    cJSON_AddStringToObject(hello, "app", "com.twitter.android");
    cJSON_AddStringToObject(hello, "plugin", "x_agent");
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
        if (length <= kMaxLine) handle_request(state, env, fd, line, length, account);
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
    if (connect(fd, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void *agent_main(void *arg) {
    AgentState *state = static_cast<AgentState *>(arg);
    JNIEnv *env = nullptr;
    if (state->host && state->host->java_vm) {
        if (state->host->java_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) env = nullptr;
    }
    LOGI("worker started pid=%d uid=%u", getpid(), static_cast<unsigned>(getuid()));
    while (!state->stop.load()) {
        std::string account = detect_account();
        if (account.empty()) {
            if (!sleep_interruptible(state, 2000)) break;
            continue;
        }
        int fd = connect_router();
        if (fd < 0) {
            if (!sleep_interruptible(state, 1000)) break;
            continue;
        }
        state->router_fd.store(fd);
        serve_router(state, env, fd, account);
        close_fd(state);
        if (!sleep_interruptible(state, 1000)) break;
    }
    if (env && state->host && state->host->java_vm) state->host->java_vm->DetachCurrentThread();
    LOGI("worker stopped");
    return nullptr;
}

/* ------------------------------------------------------------------------- */
/* DejaVu plugin lifecycle                                                   */

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
    if (state->thread_started) {
        pthread_join(state->thread, nullptr);
        state->thread_started = false;
    }
}

void plugin_control_ready(void *plugin_state) {
    auto *state = static_cast<AgentState *>(plugin_state);
    if (state && state->host && state->host->log_message) {
        state->host->log_message(state->host->host_context, ANDROID_LOG_INFO,
                                 "x-agent ready: route is the current X account id");
    }
}

void plugin_unload_prepare(void *plugin_state) {
    plugin_stop(static_cast<AgentState *>(plugin_state));
}

void plugin_shutdown(void *plugin_state) {
    auto *state = static_cast<AgentState *>(plugin_state);
    if (!state) return;
    plugin_stop(state);
    g_state.store(nullptr);
    delete state;
    LOGI("plugin shutdown");
}

}  // namespace

extern "C" __attribute__((visibility("default"))) const DejavuPluginDescriptor *
dejavu_plugin_entry(void) {
    static DejavuPluginDescriptor descriptor = {
        DEJAVU_PLUGIN_ABI_VERSION,
        sizeof(DejavuPluginDescriptor),
        "x_agent",
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