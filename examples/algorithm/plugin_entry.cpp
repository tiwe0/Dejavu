#include "dejavu_plugin.h"
#include "rpc_helper.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr size_t kMaximumData = 1024;
constexpr size_t kMaxRecentEvents = 128;
constexpr size_t kDefaultRpcRecentLimit = 32;
constexpr size_t kMaxRpcRecentLimit = 64;

struct InstalledHook {
    void *target = nullptr;
    std::string symbol;
};

struct TraceEvent {
    unsigned long long seq = 0;
    std::string line;
};

struct AlgorithmState {
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
    std::vector<InstalledHook> hooks;
    unsigned long long events = 0;
    unsigned long long bytes = 0;
    unsigned long long event_seq = 0;
    unsigned long long dropped_events = 0;
    std::deque<TraceEvent> recent;
    const char *(*md_name)(const void *) = nullptr;
    const char *(*cipher_name)(const void *) = nullptr;
    int (*cipher_key_length)(const void *) = nullptr;
};

AlgorithmState *g_state = nullptr;

int (*orig_digest_init)(void *, const void *, void *) = nullptr;
int (*orig_digest_update)(void *, const void *, size_t) = nullptr;
int (*orig_digest_final)(void *, unsigned char *, unsigned int *) = nullptr;
int (*orig_hmac_init)(void *, const void *, int, const void *, void *) = nullptr;
int (*orig_hmac_update)(void *, const unsigned char *, size_t) = nullptr;
int (*orig_hmac_final)(void *, unsigned char *, unsigned int *) = nullptr;
int (*orig_encrypt_init)(void *, const void *, void *, const unsigned char *,
                         const unsigned char *) = nullptr;
int (*orig_encrypt_update)(void *, unsigned char *, int *, const unsigned char *, int) = nullptr;
int (*orig_encrypt_final)(void *, unsigned char *, int *) = nullptr;
int (*orig_decrypt_init)(void *, const void *, void *, const unsigned char *,
                         const unsigned char *) = nullptr;
int (*orig_decrypt_update)(void *, unsigned char *, int *, const unsigned char *, int) = nullptr;
int (*orig_decrypt_final)(void *, unsigned char *, int *) = nullptr;
int (*orig_sign_update)(void *, const void *, size_t) = nullptr;
int (*orig_sign_final)(void *, unsigned char *, size_t *) = nullptr;
int (*orig_verify_update)(void *, const void *, size_t) = nullptr;
int (*orig_verify_final)(void *, const unsigned char *, size_t) = nullptr;

char *make_string(AlgorithmState *state, const std::string &text) {
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

void set_result_string(AlgorithmState *state, dejavu_value *result, const std::string &text) {
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

std::string pointer_text(const void *pointer) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%p", pointer);
    return buffer;
}

std::string hex_text(const void *data, size_t size) {
    if (data == nullptr || size == 0) {
        return "-";
    }
    const size_t limit = size < kMaximumData ? size : kMaximumData;
    static const char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(limit * 2);
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (size_t index = 0; index < limit; ++index) {
        output.push_back(digits[bytes[index] >> 4]);
        output.push_back(digits[bytes[index] & 0x0F]);
    }
    if (size > limit) {
        output += "...";
    }
    return output;
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

void emit(AlgorithmState *state, const std::string &line) {
    if (state == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->active) {
        return;
    }
    std::string record = line;
    record.push_back('\n');
    if (state->fd >= 0) {
        write_all(state->fd, record.data(), record.size());
    }
    if (state->stream_handle > 0 && state->stream != nullptr) {
        state->stream->write(state->host->host_context, state->stream_handle, record.data(),
                             record.size());
    }
    if (state->recent.size() >= kMaxRecentEvents) {
        state->recent.pop_front();
        ++state->dropped_events;
    }
    state->recent.push_back(TraceEvent{++state->event_seq, line});
    ++state->events;
    state->bytes += record.size();
}

std::string algorithm_suffix(AlgorithmState *state, const void *md, const void *cipher) {
    std::string suffix;
    if (md != nullptr && state->md_name != nullptr) {
        const char *name = state->md_name(md);
        if (name != nullptr) {
            suffix += " algo=";
            suffix += name;
        }
    }
    if (cipher != nullptr && state->cipher_name != nullptr) {
        const char *name = state->cipher_name(cipher);
        if (name != nullptr) {
            suffix += " algo=";
            suffix += name;
        }
    }
    return suffix;
}

int hook_digest_init(void *ctx, const void *md, void *engine) {
    AlgorithmState *state = g_state;
    const int result = orig_digest_init != nullptr ? orig_digest_init(ctx, md, engine) : -1;
    if (state != nullptr) {
        emit(state, "DIGEST_INIT ctx=" + pointer_text(ctx) + algorithm_suffix(state, md, nullptr));
    }
    return result;
}

int hook_digest_update(void *ctx, const void *data, size_t size) {
    AlgorithmState *state = g_state;
    if (state != nullptr) {
        emit(state, "DIGEST_UPDATE ctx=" + pointer_text(ctx) + " len=" + std::to_string(size) +
                        " data=" + hex_text(data, size));
    }
    return orig_digest_update != nullptr ? orig_digest_update(ctx, data, size) : -1;
}

int hook_digest_final(void *ctx, unsigned char *md, unsigned int *size) {
    AlgorithmState *state = g_state;
    const int result = orig_digest_final != nullptr ? orig_digest_final(ctx, md, size) : -1;
    if (state != nullptr && result == 1 && md != nullptr && size != nullptr) {
        emit(state, "DIGEST_FINAL ctx=" + pointer_text(ctx) + " len=" + std::to_string(*size) +
                        " data=" + hex_text(md, *size));
    }
    return result;
}

int hook_hmac_init(void *ctx, const void *key, int key_length, const void *md, void *engine) {
    AlgorithmState *state = g_state;
    const int result =
        orig_hmac_init != nullptr ? orig_hmac_init(ctx, key, key_length, md, engine) : -1;
    if (state != nullptr && key != nullptr && key_length > 0) {
        emit(state, "HMAC_INIT ctx=" + pointer_text(ctx) + algorithm_suffix(state, md, nullptr) +
                        " key=" + hex_text(key, static_cast<size_t>(key_length)));
    }
    return result;
}

int hook_hmac_update(void *ctx, const unsigned char *data, size_t size) {
    AlgorithmState *state = g_state;
    if (state != nullptr) {
        emit(state, "HMAC_UPDATE ctx=" + pointer_text(ctx) + " len=" + std::to_string(size) +
                        " data=" + hex_text(data, size));
    }
    return orig_hmac_update != nullptr ? orig_hmac_update(ctx, data, size) : -1;
}

int hook_hmac_final(void *ctx, unsigned char *md, unsigned int *size) {
    AlgorithmState *state = g_state;
    const int result = orig_hmac_final != nullptr ? orig_hmac_final(ctx, md, size) : -1;
    if (state != nullptr && result == 1 && md != nullptr && size != nullptr) {
        emit(state, "HMAC_FINAL ctx=" + pointer_text(ctx) + " len=" + std::to_string(*size) +
                        " data=" + hex_text(md, *size));
    }
    return result;
}

int hook_encrypt_init(void *ctx, const void *cipher, void *engine, const unsigned char *key,
                      const unsigned char *iv) {
    AlgorithmState *state = g_state;
    const int result =
        orig_encrypt_init != nullptr ? orig_encrypt_init(ctx, cipher, engine, key, iv) : -1;
    if (state != nullptr) {
        std::string line =
            "CIPHER_ENC_INIT ctx=" + pointer_text(ctx) + algorithm_suffix(state, nullptr, cipher);
        if (key != nullptr && state->cipher_key_length != nullptr) {
            const int key_length = state->cipher_key_length(cipher);
            if (key_length > 0) {
                line += " key=" + hex_text(key, static_cast<size_t>(key_length));
            }
        }
        if (iv != nullptr) {
            line += " iv=" + hex_text(iv, 16);
        }
        emit(state, line);
    }
    return result;
}

int hook_encrypt_update(void *ctx, unsigned char *out, int *out_length, const unsigned char *in,
                        int in_length) {
    AlgorithmState *state = g_state;
    if (state != nullptr && in_length > 0) {
        emit(state, "CIPHER_ENC_UPDATE ctx=" + pointer_text(ctx) +
                        " in_len=" + std::to_string(in_length) +
                        " in=" + hex_text(in, static_cast<size_t>(in_length)));
    }
    const int result = orig_encrypt_update != nullptr
                           ? orig_encrypt_update(ctx, out, out_length, in, in_length)
                           : -1;
    if (state != nullptr && result == 1 && out_length != nullptr && *out_length > 0) {
        emit(state, "CIPHER_ENC_OUT ctx=" + pointer_text(ctx) +
                        " len=" + std::to_string(*out_length) +
                        " data=" + hex_text(out, static_cast<size_t>(*out_length)));
    }
    return result;
}

int hook_encrypt_final(void *ctx, unsigned char *out, int *out_length) {
    const int result =
        orig_encrypt_final != nullptr ? orig_encrypt_final(ctx, out, out_length) : -1;
    AlgorithmState *state = g_state;
    if (state != nullptr && result == 1 && out_length != nullptr && *out_length > 0) {
        emit(state, "CIPHER_ENC_FINAL ctx=" + pointer_text(ctx) +
                        " len=" + std::to_string(*out_length) +
                        " data=" + hex_text(out, static_cast<size_t>(*out_length)));
    }
    return result;
}

int hook_decrypt_init(void *ctx, const void *cipher, void *engine, const unsigned char *key,
                      const unsigned char *iv) {
    AlgorithmState *state = g_state;
    const int result =
        orig_decrypt_init != nullptr ? orig_decrypt_init(ctx, cipher, engine, key, iv) : -1;
    if (state != nullptr) {
        std::string line =
            "CIPHER_DEC_INIT ctx=" + pointer_text(ctx) + algorithm_suffix(state, nullptr, cipher);
        if (key != nullptr && state->cipher_key_length != nullptr) {
            const int key_length = state->cipher_key_length(cipher);
            if (key_length > 0) {
                line += " key=" + hex_text(key, static_cast<size_t>(key_length));
            }
        }
        if (iv != nullptr) {
            line += " iv=" + hex_text(iv, 16);
        }
        emit(state, line);
    }
    return result;
}

int hook_decrypt_update(void *ctx, unsigned char *out, int *out_length, const unsigned char *in,
                        int in_length) {
    AlgorithmState *state = g_state;
    if (state != nullptr && in_length > 0) {
        emit(state, "CIPHER_DEC_UPDATE ctx=" + pointer_text(ctx) +
                        " in_len=" + std::to_string(in_length) +
                        " in=" + hex_text(in, static_cast<size_t>(in_length)));
    }
    const int result = orig_decrypt_update != nullptr
                           ? orig_decrypt_update(ctx, out, out_length, in, in_length)
                           : -1;
    if (state != nullptr && result == 1 && out_length != nullptr && *out_length > 0) {
        emit(state, "CIPHER_DEC_OUT ctx=" + pointer_text(ctx) +
                        " len=" + std::to_string(*out_length) +
                        " data=" + hex_text(out, static_cast<size_t>(*out_length)));
    }
    return result;
}

int hook_decrypt_final(void *ctx, unsigned char *out, int *out_length) {
    const int result =
        orig_decrypt_final != nullptr ? orig_decrypt_final(ctx, out, out_length) : -1;
    AlgorithmState *state = g_state;
    if (state != nullptr && result == 1 && out_length != nullptr && *out_length > 0) {
        emit(state, "CIPHER_DEC_FINAL ctx=" + pointer_text(ctx) +
                        " len=" + std::to_string(*out_length) +
                        " data=" + hex_text(out, static_cast<size_t>(*out_length)));
    }
    return result;
}

int hook_sign_update(void *ctx, const void *data, size_t size) {
    AlgorithmState *state = g_state;
    if (state != nullptr) {
        emit(state, "SIGN_UPDATE ctx=" + pointer_text(ctx) + " len=" + std::to_string(size) +
                        " data=" + hex_text(data, size));
    }
    return orig_sign_update != nullptr ? orig_sign_update(ctx, data, size) : -1;
}

int hook_sign_final(void *ctx, unsigned char *signature, size_t *size) {
    AlgorithmState *state = g_state;
    const int result = orig_sign_final != nullptr ? orig_sign_final(ctx, signature, size) : -1;
    if (state != nullptr && result == 1 && signature != nullptr && size != nullptr) {
        emit(state, "SIGN_FINAL ctx=" + pointer_text(ctx) + " len=" + std::to_string(*size) +
                        " data=" + hex_text(signature, *size));
    }
    return result;
}

int hook_verify_update(void *ctx, const void *data, size_t size) {
    return orig_verify_update != nullptr ? orig_verify_update(ctx, data, size) : -1;
}

int hook_verify_final(void *ctx, const unsigned char *signature, size_t size) {
    const int result = orig_verify_final != nullptr ? orig_verify_final(ctx, signature, size) : -1;
    AlgorithmState *state = g_state;
    if (state != nullptr) {
        emit(state, "VERIFY_FINAL ctx=" + pointer_text(ctx) + " result=" + std::to_string(result) +
                        " sig_len=" + std::to_string(size) + " sig=" + hex_text(signature, size));
    }
    return result;
}

void *resolve_any(AlgorithmState *state, const char *symbol) {
    static const char *libraries[] = {
        "/apex/com.android.conscrypt/lib64/libcrypto.so",
        "/system/lib64/libcrypto.so",
        "libcrypto.so",
        "libssl.so",
        "libjavacrypto.so",
        "",
    };
    for (const char *library : libraries) {
        void *address = state->module->resolve(state->host->host_context, library, symbol);
        if (address != nullptr) {
            return address;
        }
    }
    return nullptr;
}

bool install_symbol(AlgorithmState *state, const char *symbol, void *replacement, void **original,
                    char *error, size_t error_size) {
    void *target = resolve_any(state, symbol);
    if (target == nullptr) {
        return false;
    }
    if (state->hook->install(state->host->host_context, target, replacement, original, error,
                             error_size) != 0) {
        return false;
    }
    state->hooks.push_back(InstalledHook{target, symbol});
    return true;
}

void clear_hooks(AlgorithmState *state) {
    std::vector<InstalledHook> hooks_copy;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        hooks_copy.swap(state->hooks);
    }
    for (const InstalledHook &hook : hooks_copy) {
        char error[256] = {};
        state->hook->uninstall(state->host->host_context, hook.target, error, sizeof(error));
    }
    orig_digest_init = nullptr;
    orig_digest_update = nullptr;
    orig_digest_final = nullptr;
    orig_hmac_init = nullptr;
    orig_hmac_update = nullptr;
    orig_hmac_final = nullptr;
    orig_encrypt_init = nullptr;
    orig_encrypt_update = nullptr;
    orig_encrypt_final = nullptr;
    orig_decrypt_init = nullptr;
    orig_decrypt_update = nullptr;
    orig_decrypt_final = nullptr;
    orig_sign_update = nullptr;
    orig_sign_final = nullptr;
    orig_verify_update = nullptr;
    orig_verify_final = nullptr;
}

bool start_algorithm(AlgorithmState *state, const std::string &path, uint32_t transport,
                     const std::string &endpoint, std::string *error, std::string *summary) {
    if (state == nullptr) {
        *error = "algorithm state is missing";
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
        if (state->active || state->fd >= 0 || state->stream_handle > 0 || !state->hooks.empty()) {
            *error = "algorithm tracing is already active";
            return false;
        }
    }
    if (state->module == nullptr || state->hook == nullptr) {
        *error = "host module/hook interface is unavailable";
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
            *error = stream_error[0] != '\0' ? stream_error : "unable to open algorithm stream";
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
            *error = "unable to open algorithm log: " + path;
            return false;
        }
    }

    state->md_name =
        reinterpret_cast<const char *(*)(const void *)>(resolve_any(state, "EVP_MD_get0_name"));
    if (state->md_name == nullptr) {
        state->md_name =
            reinterpret_cast<const char *(*)(const void *)>(resolve_any(state, "EVP_MD_name"));
    }
    state->cipher_name =
        reinterpret_cast<const char *(*)(const void *)>(resolve_any(state, "EVP_CIPHER_get0_name"));
    if (state->cipher_name == nullptr) {
        state->cipher_name =
            reinterpret_cast<const char *(*)(const void *)>(resolve_any(state, "EVP_CIPHER_name"));
    }
    state->cipher_key_length =
        reinterpret_cast<int (*)(const void *)>(resolve_any(state, "EVP_CIPHER_get_key_length"));
    if (state->cipher_key_length == nullptr) {
        state->cipher_key_length =
            reinterpret_cast<int (*)(const void *)>(resolve_any(state, "EVP_CIPHER_key_length"));
    }

    char hook_error[256] = {};
    int installed = 0;
    auto try_install = [&](const char *symbol, void *replacement, void **original) {
        if (install_symbol(state, symbol, replacement, original, hook_error, sizeof(hook_error))) {
            ++installed;
        }
    };
    try_install("EVP_DigestInit_ex", reinterpret_cast<void *>(&hook_digest_init),
                reinterpret_cast<void **>(&orig_digest_init));
    try_install("EVP_DigestUpdate", reinterpret_cast<void *>(&hook_digest_update),
                reinterpret_cast<void **>(&orig_digest_update));
    try_install("EVP_DigestFinal_ex", reinterpret_cast<void *>(&hook_digest_final),
                reinterpret_cast<void **>(&orig_digest_final));
    try_install("HMAC_Init_ex", reinterpret_cast<void *>(&hook_hmac_init),
                reinterpret_cast<void **>(&orig_hmac_init));
    try_install("HMAC_Update", reinterpret_cast<void *>(&hook_hmac_update),
                reinterpret_cast<void **>(&orig_hmac_update));
    try_install("HMAC_Final", reinterpret_cast<void *>(&hook_hmac_final),
                reinterpret_cast<void **>(&orig_hmac_final));
    try_install("EVP_EncryptInit_ex", reinterpret_cast<void *>(&hook_encrypt_init),
                reinterpret_cast<void **>(&orig_encrypt_init));
    try_install("EVP_EncryptUpdate", reinterpret_cast<void *>(&hook_encrypt_update),
                reinterpret_cast<void **>(&orig_encrypt_update));
    try_install("EVP_EncryptFinal_ex", reinterpret_cast<void *>(&hook_encrypt_final),
                reinterpret_cast<void **>(&orig_encrypt_final));
    try_install("EVP_DecryptInit_ex", reinterpret_cast<void *>(&hook_decrypt_init),
                reinterpret_cast<void **>(&orig_decrypt_init));
    try_install("EVP_DecryptUpdate", reinterpret_cast<void *>(&hook_decrypt_update),
                reinterpret_cast<void **>(&orig_decrypt_update));
    try_install("EVP_DecryptFinal_ex", reinterpret_cast<void *>(&hook_decrypt_final),
                reinterpret_cast<void **>(&orig_decrypt_final));
    try_install("EVP_DigestSignUpdate", reinterpret_cast<void *>(&hook_sign_update),
                reinterpret_cast<void **>(&orig_sign_update));
    try_install("EVP_DigestSignFinal", reinterpret_cast<void *>(&hook_sign_final),
                reinterpret_cast<void **>(&orig_sign_final));
    try_install("EVP_DigestVerifyUpdate", reinterpret_cast<void *>(&hook_verify_update),
                reinterpret_cast<void **>(&orig_verify_update));
    try_install("EVP_DigestVerifyFinal", reinterpret_cast<void *>(&hook_verify_final),
                reinterpret_cast<void **>(&orig_verify_final));
    if (installed == 0) {
        if (fd >= 0) {
            close(fd);
        }
        if (stream_handle > 0) {
            state->stream->close(state->host->host_context, stream_handle);
        }
        *error = "no libcrypto algorithm symbols were hookable";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->fd = fd;
        state->stream_handle = stream_handle;
        state->path = path;
        state->active = true;
    }
    g_state = state;
    if (summary != nullptr) {
        *summary = "algorithm tracing started hooks=" + std::to_string(installed) +
                   (path.empty() ? (stream_handle > 0 ? "" : " sink=memory") : " file=" + path) +
                   (stream_handle > 0 ? " stream=" + std::to_string(stream_handle) : "");
    }
    return true;
}

bool stop_algorithm(AlgorithmState *state, std::string *error) {
    if (state == nullptr) {
        *error = "algorithm state is missing";
        return false;
    }
    if (g_state == state) {
        g_state = nullptr;
    }
    clear_hooks(state);
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
    return true;
}

bool run_selftest(AlgorithmState *state, std::string *digest_out, std::string *error) {
    if (state == nullptr || state->module == nullptr) {
        *error = "algorithm module interface is unavailable";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->hooks.empty()) {
            *error = "algorithm.start must install hooks first";
            return false;
        }
    }
    using CtxNew = void *(*)();
    using CtxFree = void (*)(void *);
    using Md5 = const void *(*)();
    using Init = int (*)(void *, const void *, void *);
    using Update = int (*)(void *, const void *, size_t);
    using Final = int (*)(void *, unsigned char *, unsigned int *);
    auto *ctx_new = reinterpret_cast<CtxNew>(resolve_any(state, "EVP_MD_CTX_new"));
    auto *ctx_free = reinterpret_cast<CtxFree>(resolve_any(state, "EVP_MD_CTX_free"));
    auto *md5 = reinterpret_cast<Md5>(resolve_any(state, "EVP_md5"));
    auto *init = reinterpret_cast<Init>(resolve_any(state, "EVP_DigestInit_ex"));
    auto *update = reinterpret_cast<Update>(resolve_any(state, "EVP_DigestUpdate"));
    auto *final = reinterpret_cast<Final>(resolve_any(state, "EVP_DigestFinal_ex"));
    if (ctx_new == nullptr || ctx_free == nullptr || md5 == nullptr || init == nullptr ||
        update == nullptr || final == nullptr) {
        *error = "EVP digest selftest symbols are unavailable";
        return false;
    }
    void *ctx = ctx_new();
    if (ctx == nullptr) {
        *error = "EVP_MD_CTX_new failed";
        return false;
    }
    unsigned char digest[64] = {};
    unsigned int digest_size = 0;
    int status = init(ctx, md5(), nullptr);
    if (status == 1) {
        status = update(ctx, "abc", 3);
    }
    if (status == 1) {
        status = final(ctx, digest, &digest_size);
    }
    ctx_free(ctx);
    if (status != 1) {
        *error = "EVP digest selftest failed";
        return false;
    }
    *digest_out = hex_text(digest, digest_size);
    return true;
}

std::string status_text(AlgorithmState *state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    std::string text = "algorithm ";
    if (!state->active) {
        return text + "idle";
    }
    if (!state->path.empty()) {
        text += state->path;
    } else if (state->stream_handle > 0) {
        text += "stream";
    } else {
        text += "memory";
    }
    text += " hooks=" + std::to_string(state->hooks.size()) +
            " events=" + std::to_string(state->events) + " bytes=" + std::to_string(state->bytes) +
            " buffered=" + std::to_string(state->recent.size());
    return text;
}

std::string status_json(AlgorithmState *state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    std::string json = "{\"service\":\"algorithm\",\"version\":\"1.1.0\",\"active\":";
    json += state->active ? "true" : "false";
    json += ",\"route\":\"" + dejavu_example::json_escape(state->rpc_route) + "\"";
    json += ",\"process\":\"" + dejavu_example::json_escape(state->process_name) + "\"";
    json += ",\"pid\":" + std::to_string(static_cast<unsigned long>(getpid()));
    json += ",\"path\":\"" + dejavu_example::json_escape(state->path) + "\"";
    json += ",\"stream_handle\":" + std::to_string(state->stream_handle);
    json += ",\"hooks\":" + std::to_string(state->hooks.size());
    json += ",\"events\":" + std::to_string(state->events);
    json += ",\"bytes\":" + std::to_string(state->bytes);
    json += ",\"buffered_events\":" + std::to_string(state->recent.size());
    json += ",\"dropped_events\":" + std::to_string(state->dropped_events);
    json += "}";
    return json;
}

int rpc_status(void *context, const DejavuRpcRequest *request, DejavuRpcResponse *response,
               char *error, size_t error_size) {
    auto *state = static_cast<AlgorithmState *>(context);
    if (dejavu_example::rpc_is_cancelled(request)) {
        return dejavu_example::rpc_reply_error(state->host, response, "algorithm.cancelled",
                                               "Request cancelled", false, error, error_size);
    }
    return dejavu_example::rpc_reply(state->host, response, true, status_json(state), error,
                                     error_size);
}

int rpc_start(void *context, const DejavuRpcRequest *request, DejavuRpcResponse *response,
              char *error, size_t error_size) {
    auto *state = static_cast<AlgorithmState *>(context);
    if (dejavu_example::rpc_is_cancelled(request)) {
        return dejavu_example::rpc_reply_error(state->host, response, "algorithm.cancelled",
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
        return dejavu_example::rpc_reply_error(state->host, response, "algorithm.invalid_argument",
                                               "transport must be 0, 1 (tcp), 2 (udp) or 3 (kcp)",
                                               false, error, error_size);
    }
    std::string start_error;
    if (!start_algorithm(state, path, static_cast<uint32_t>(transport), endpoint, &start_error,
                         nullptr)) {
        const char *code = start_error == "algorithm tracing is already active"
                               ? "algorithm.already_active"
                               : "algorithm.start_failed";
        return dejavu_example::rpc_reply_error(state->host, response, code, start_error, false,
                                               error, error_size);
    }
    return dejavu_example::rpc_reply(state->host, response, true, status_json(state), error,
                                     error_size);
}

int rpc_stop(void *context, const DejavuRpcRequest *request, DejavuRpcResponse *response,
             char *error, size_t error_size) {
    auto *state = static_cast<AlgorithmState *>(context);
    if (dejavu_example::rpc_is_cancelled(request)) {
        return dejavu_example::rpc_reply_error(state->host, response, "algorithm.cancelled",
                                               "Request cancelled", false, error, error_size);
    }
    std::string stop_error;
    if (!stop_algorithm(state, &stop_error)) {
        return dejavu_example::rpc_reply_error(state->host, response, "algorithm.stop_failed",
                                               stop_error, false, error, error_size);
    }
    return dejavu_example::rpc_reply(state->host, response, true, status_json(state), error,
                                     error_size);
}

int rpc_recent(void *context, const DejavuRpcRequest *request, DejavuRpcResponse *response,
               char *error, size_t error_size) {
    auto *state = static_cast<AlgorithmState *>(context);
    if (dejavu_example::rpc_is_cancelled(request)) {
        return dejavu_example::rpc_reply_error(state->host, response, "algorithm.cancelled",
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
        json += ",\"dropped_events\":" + std::to_string(state->dropped_events);
        json += ",\"events\":[";
        for (size_t i = 0; i < take; ++i) {
            const TraceEvent &ev = state->recent[start_index + i];
            if (i > 0) {
                json.push_back(',');
            }
            json += "{\"seq\":" + std::to_string(ev.seq);
            json += ",\"line\":\"" + dejavu_example::json_escape(ev.line) + "\"}";
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
    auto *state = static_cast<AlgorithmState *>(context);
    if (dejavu_example::rpc_is_cancelled(request)) {
        return dejavu_example::rpc_reply_error(state->host, response, "algorithm.cancelled",
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

int rpc_selftest(void *context, const DejavuRpcRequest *request, DejavuRpcResponse *response,
                 char *error, size_t error_size) {
    auto *state = static_cast<AlgorithmState *>(context);
    if (dejavu_example::rpc_is_cancelled(request)) {
        return dejavu_example::rpc_reply_error(state->host, response, "algorithm.cancelled",
                                               "Request cancelled", false, error, error_size);
    }
    std::string digest;
    std::string test_error;
    if (!run_selftest(state, &digest, &test_error)) {
        return dejavu_example::rpc_reply_error(state->host, response, "algorithm.selftest_failed",
                                               test_error, false, error, error_size);
    }
    unsigned long long events = 0;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        events = state->events;
    }
    const std::string json = "{\"digest\":\"" + dejavu_example::json_escape(digest) +
                             "\",\"events\":" + std::to_string(events) + "}";
    return dejavu_example::rpc_reply(state->host, response, true, json, error, error_size);
}

int lua_start(dejavu_control_session *, void *context, const dejavu_value *arguments,
              size_t argument_count, dejavu_value *result, char *error, size_t error_size) {
    auto *state = static_cast<AlgorithmState *>(context);
    if (state == nullptr ||
        (argument_count != 0 && argument_count != 1 && argument_count != 3)) {
        std::snprintf(
            error, error_size,
            "algorithm.start expects (), <path>, or <path>, <1=tcp|2=udp|3=kcp>, <endpoint>");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::string path;
    if (argument_count >= 1) {
        if (arguments[0].type != DEJAVU_VALUE_STRING || arguments[0].string_value == nullptr) {
            std::snprintf(error, error_size, "algorithm.start path must be a string");
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
            std::snprintf(error, error_size, "transport must be 1/2/3 and endpoint a string");
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
    if (!start_algorithm(state, path, transport, endpoint, &start_error, &summary)) {
        std::snprintf(error, error_size, "%s", start_error.c_str());
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    set_result_string(state, result, summary);
    return DEJAVU_OK;
}

int lua_stop(dejavu_control_session *, void *context, const dejavu_value *, size_t,
             dejavu_value *result, char *error, size_t error_size) {
    auto *state = static_cast<AlgorithmState *>(context);
    std::string stop_error;
    if (!stop_algorithm(state, &stop_error)) {
        std::snprintf(error, error_size, "%s", stop_error.c_str());
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    set_result_string(state, result, "algorithm tracing stopped");
    return DEJAVU_OK;
}

int lua_selftest(dejavu_control_session *, void *context, const dejavu_value *, size_t,
                 dejavu_value *result, char *error, size_t error_size) {
    auto *state = static_cast<AlgorithmState *>(context);
    std::string digest;
    std::string test_error;
    if (!run_selftest(state, &digest, &test_error)) {
        std::snprintf(error, error_size, "%s", test_error.c_str());
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    set_result_string(state, result, digest);
    return DEJAVU_OK;
}

int lua_status(dejavu_control_session *, void *context, const dejavu_value *, size_t,
               dejavu_value *result, char *, size_t) {
    auto *state = static_cast<AlgorithmState *>(context);
    set_result_string(state, result, status_text(state));
    return DEJAVU_OK;
}

void unregister_rpc(AlgorithmState *state) {
    if (state != nullptr && state->rpc != nullptr && state->rpc_service != 0 &&
        state->rpc->unregister_service != nullptr) {
        char rpc_error[128] = {};
        state->rpc->unregister_service(state->host->host_context, state->rpc_service, rpc_error,
                                       sizeof(rpc_error));
        state->rpc_service = 0;
    }
}

bool register_rpc(AlgorithmState *state, char *error, size_t error_size) {
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
        {"selftest", rpc_selftest},
    };
    const DejavuRpcServiceDescriptor descriptor = {
        sizeof(DejavuRpcServiceDescriptor),
        "algorithm",
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
    auto *state = new AlgorithmState();
    state->host = host;
    state->process_name = dejavu_example::current_process_name();
    state->rpc_route = dejavu_example::make_process_route("algorithm");
    state->module = static_cast<const DejavuModuleInterface *>(
        host->get_interface(host->host_context, "module", DEJAVU_MODULE_INTERFACE_VERSION));
    state->hook = static_cast<const DejavuHookInterface *>(
        host->get_interface(host->host_context, "hook", DEJAVU_HOOK_INTERFACE_VERSION));
    state->stream = static_cast<const DejavuStreamInterface *>(
        host->get_interface(host->host_context, "stream", DEJAVU_STREAM_INTERFACE_VERSION));
    if (state->module == nullptr || state->hook == nullptr) {
        std::snprintf(error, error_size, "algorithm requires module and hook host interfaces");
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
        {"selftest", lua_selftest},
    };
    for (const auto &registration : registrations) {
        if (host->register_function(host->host_context, "algorithm", registration.name,
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
    auto *state = static_cast<AlgorithmState *>(plugin_state);
    if (state == nullptr) {
        return;
    }
    unregister_rpc(state);
    std::string error;
    if (!stop_algorithm(state, &error) && state->host != nullptr &&
        state->host->log_message != nullptr) {
        state->host->log_message(state->host->host_context, 5,
                                 ("algorithm stop failed: " + error).c_str());
    }
}

void plugin_shutdown(void *plugin_state) {
    auto *state = static_cast<AlgorithmState *>(plugin_state);
    if (state == nullptr) {
        return;
    }
    unregister_rpc(state);
    std::string error;
    stop_algorithm(state, &error);
    delete state;
}

} // namespace

extern "C" __attribute__((visibility("default"))) const DejavuPluginDescriptor *
dejavu_plugin_entry(void) {
    static DejavuPluginDescriptor descriptor = {
        DEJAVU_PLUGIN_ABI_VERSION,
        sizeof(DejavuPluginDescriptor),
        "algorithm",
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
