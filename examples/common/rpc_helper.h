#pragma once

#include "dejavu_plugin.h"

#include <fcntl.h>
#include <unistd.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace dejavu_example {

inline std::string current_process_name() {
#if defined(__linux__)
    const int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        char buf[256] = {};
        const ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            std::string raw(buf);
            if (!raw.empty()) {
                const size_t slash = raw.find_last_of('/');
                if (slash != std::string::npos && slash + 1 < raw.size()) {
                    raw = raw.substr(slash + 1);
                }
                if (!raw.empty()) {
                    return raw;
                }
            }
        }
    }
#endif
    return "pid." + std::to_string(static_cast<unsigned long>(getpid()));
}

inline std::string sanitize_route_segment(const std::string &raw, size_t max_len) {
    std::string out;
    out.reserve(raw.size() < max_len ? raw.size() : max_len);
    for (unsigned char ch : raw) {
        if (out.size() >= max_len) {
            break;
        }
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '.' || ch == '-') {
            out.push_back(static_cast<char>(ch));
        } else if (ch == ':') {
            out.push_back('.');
        } else if (ch >= 0x20 && ch < 0x7f) {
            out.push_back('_');
        }
    }
    while (!out.empty() && (out.front() == '.' || out.front() == '-')) {
        out.erase(out.begin());
    }
    while (!out.empty() && (out.back() == '.' || out.back() == '-')) {
        out.pop_back();
    }
    if (out.empty()) {
        out = "pid." + std::to_string(static_cast<unsigned long>(getpid()));
        if (out.size() > max_len) {
            out.resize(max_len);
        }
    }
    return out;
}

inline std::string make_process_route(const std::string &prefix) {
    constexpr size_t kMaxRouteBytes = 128;
    if (prefix.size() + 2 >= kMaxRouteBytes) {
        return prefix.substr(0, kMaxRouteBytes);
    }
    const size_t remaining = kMaxRouteBytes - prefix.size() - 1;
    return prefix + "." + sanitize_route_segment(current_process_name(), remaining);
}

inline std::string json_escape(const std::string &input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (unsigned char ch : input) {
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (ch < 0x20 || ch == 0x7f) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(ch));
                out += buf;
            } else {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return out;
}

inline int rpc_reply(const DejavuPluginHost *host, DejavuRpcResponse *response, bool ok,
                     const std::string &json, char *error, size_t error_size) {
    if (host == nullptr || host->alloc == nullptr || response == nullptr) {
        if (error != nullptr && error_size > 0) {
            std::snprintf(error, error_size, "host allocator unavailable");
        }
        return DEJAVU_RPC_ERROR;
    }
    auto *copy = static_cast<char *>(host->alloc(json.size() + 1));
    if (copy == nullptr) {
        if (error != nullptr && error_size > 0) {
            std::snprintf(error, error_size, "unable to allocate RPC response");
        }
        return DEJAVU_RPC_ERROR;
    }
    std::memcpy(copy, json.data(), json.size());
    copy[json.size()] = '\0';
    response->struct_size = sizeof(*response);
    response->ok = ok ? 1 : 0;
    response->payload_json = copy;
    response->payload_size = json.size();
    return DEJAVU_RPC_OK;
}

inline int rpc_reply_error(const DejavuPluginHost *host, DejavuRpcResponse *response,
                           const char *code, const std::string &message, bool retryable,
                           char *error, size_t error_size) {
    std::string payload = "{\"code\":\"";
    payload += json_escape(code != nullptr ? code : "plugin.error");
    payload += "\",\"message\":\"";
    payload += json_escape(message);
    payload += "\",\"retryable\":";
    payload += retryable ? "true}" : "false}";
    return rpc_reply(host, response, false, payload, error, error_size);
}

inline bool rpc_is_cancelled(const DejavuRpcRequest *request) {
    return request != nullptr && request->is_cancelled != nullptr &&
           request->is_cancelled(request->cancellation_context) != 0;
}

namespace detail {

struct JsonCursor {
    const char *data = nullptr;
    size_t size = 0;
    size_t pos = 0;

    void skip_ws() {
        while (pos < size && (data[pos] == ' ' || data[pos] == '\t' || data[pos] == '\r' ||
                              data[pos] == '\n')) {
            ++pos;
        }
    }

    bool parse_string(std::string *out) {
        skip_ws();
        if (pos >= size || data[pos] != '"') {
            return false;
        }
        ++pos;
        std::string result;
        while (pos < size) {
            const char ch = data[pos++];
            if (ch == '"') {
                if (out != nullptr) {
                    *out = std::move(result);
                }
                return true;
            }
            if (ch == '\\') {
                if (pos >= size) {
                    return false;
                }
                const char esc = data[pos++];
                switch (esc) {
                case '"':
                case '\\':
                case '/':
                    result.push_back(esc);
                    break;
                case 'b':
                    result.push_back('\b');
                    break;
                case 'f':
                    result.push_back('\f');
                    break;
                case 'n':
                    result.push_back('\n');
                    break;
                case 'r':
                    result.push_back('\r');
                    break;
                case 't':
                    result.push_back('\t');
                    break;
                case 'u': {
                    if (pos + 4 > size) {
                        return false;
                    }
                    unsigned int code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char hex = data[pos++];
                        code <<= 4;
                        if (hex >= '0' && hex <= '9') {
                            code |= static_cast<unsigned int>(hex - '0');
                        } else if (hex >= 'a' && hex <= 'f') {
                            code |= static_cast<unsigned int>(hex - 'a' + 10);
                        } else if (hex >= 'A' && hex <= 'F') {
                            code |= static_cast<unsigned int>(hex - 'A' + 10);
                        } else {
                            return false;
                        }
                    }
                    if (code <= 0x7f) {
                        result.push_back(static_cast<char>(code));
                    } else if (code <= 0x7ff) {
                        result.push_back(static_cast<char>(0xc0 | (code >> 6)));
                        result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                    } else {
                        result.push_back(static_cast<char>(0xe0 | (code >> 12)));
                        result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                        result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                    }
                    break;
                }
                default:
                    return false;
                }
            } else {
                result.push_back(ch);
            }
        }
        return false;
    }

    bool skip_value(int depth = 0) {
        if (depth > 32) {
            return false;
        }
        skip_ws();
        if (pos >= size) {
            return false;
        }
        const char ch = data[pos];
        if (ch == '"') {
            return parse_string(nullptr);
        }
        if (ch == '{') {
            ++pos;
            skip_ws();
            if (pos < size && data[pos] == '}') {
                ++pos;
                return true;
            }
            while (pos < size) {
                if (!parse_string(nullptr)) {
                    return false;
                }
                skip_ws();
                if (pos >= size || data[pos] != ':') {
                    return false;
                }
                ++pos;
                if (!skip_value(depth + 1)) {
                    return false;
                }
                skip_ws();
                if (pos < size && data[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (pos < size && data[pos] == '}') {
                    ++pos;
                    return true;
                }
                return false;
            }
            return false;
        }
        if (ch == '[') {
            ++pos;
            skip_ws();
            if (pos < size && data[pos] == ']') {
                ++pos;
                return true;
            }
            while (pos < size) {
                if (!skip_value(depth + 1)) {
                    return false;
                }
                skip_ws();
                if (pos < size && data[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (pos < size && data[pos] == ']') {
                    ++pos;
                    return true;
                }
                return false;
            }
            return false;
        }
        while (pos < size && data[pos] != ',' && data[pos] != '}' && data[pos] != ']' &&
               data[pos] != ' ' && data[pos] != '\t' && data[pos] != '\r' && data[pos] != '\n') {
            ++pos;
        }
        return true;
    }

    bool find_top_level_field(const char *target_key) {
        skip_ws();
        if (pos >= size || data[pos] != '{') {
            return false;
        }
        ++pos;
        skip_ws();
        if (pos < size && data[pos] == '}') {
            return false;
        }
        while (pos < size) {
            std::string key;
            if (!parse_string(&key)) {
                return false;
            }
            skip_ws();
            if (pos >= size || data[pos] != ':') {
                return false;
            }
            ++pos;
            skip_ws();
            if (key == target_key) {
                return true;
            }
            if (!skip_value(1)) {
                return false;
            }
            skip_ws();
            if (pos < size && data[pos] == ',') {
                ++pos;
                continue;
            }
            return false;
        }
        return false;
    }
};

} // namespace detail

inline bool json_get_string(const char *json, size_t size, const char *key, std::string *out) {
    if (json == nullptr || size == 0 || key == nullptr || out == nullptr) {
        return false;
    }
    detail::JsonCursor cursor{json, size, 0};
    if (!cursor.find_top_level_field(key)) {
        return false;
    }
    return cursor.parse_string(out);
}

inline bool json_get_int64(const char *json, size_t size, const char *key, int64_t *out) {
    if (json == nullptr || size == 0 || key == nullptr || out == nullptr) {
        return false;
    }
    detail::JsonCursor cursor{json, size, 0};
    if (!cursor.find_top_level_field(key)) {
        return false;
    }
    cursor.skip_ws();
    if (cursor.pos >= cursor.size) {
        return false;
    }
    const size_t start = cursor.pos;
    if (cursor.data[cursor.pos] == '-') {
        ++cursor.pos;
    }
    const size_t digits_start = cursor.pos;
    while (cursor.pos < cursor.size &&
           std::isdigit(static_cast<unsigned char>(cursor.data[cursor.pos]))) {
        ++cursor.pos;
    }
    if (cursor.pos == digits_start) {
        return false;
    }
    // Reject floats like 1.5 or 1e3 when an integer is expected.
    if (cursor.pos < cursor.size &&
        (cursor.data[cursor.pos] == '.' || cursor.data[cursor.pos] == 'e' ||
         cursor.data[cursor.pos] == 'E')) {
        return false;
    }
    std::string token(cursor.data + start, cursor.pos - start);
    char *end = nullptr;
    const long long value = std::strtoll(token.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    *out = static_cast<int64_t>(value);
    return true;
}

inline bool json_get_bool(const char *json, size_t size, const char *key, bool *out) {
    if (json == nullptr || size == 0 || key == nullptr || out == nullptr) {
        return false;
    }
    detail::JsonCursor cursor{json, size, 0};
    if (!cursor.find_top_level_field(key)) {
        return false;
    }
    cursor.skip_ws();
    if (cursor.pos + 4 <= cursor.size && std::memcmp(cursor.data + cursor.pos, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (cursor.pos + 5 <= cursor.size && std::memcmp(cursor.data + cursor.pos, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

} // namespace dejavu_example
