#include "hook_compile_diagnostics.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string_view>
#include <vector>

namespace {

bool is_decimal(std::string_view text) {
    return !text.empty() &&
           std::all_of(text.begin(), text.end(), [](unsigned char ch) {
               return std::isdigit(ch) != 0;
           });
}

bool parse_decimal(std::string_view text, size_t *value) {
    size_t parsed = 0;
    if (!is_decimal(text)) {
        return false;
    }
    for (unsigned char ch : text) {
        if (parsed > (std::numeric_limits<size_t>::max() - (ch - '0')) / 10) {
            return false;
        }
        parsed = parsed * 10 + static_cast<size_t>(ch - '0');
    }
    if (value != nullptr) {
        *value = parsed;
    }
    return true;
}

bool looks_like_jni_identifier(std::string_view identifier) {
    return identifier == "JNIEnv" || identifier == "JavaVM" ||
           (identifier.size() > 1 && identifier.front() == 'j' &&
            std::isalpha(static_cast<unsigned char>(identifier[1])) != 0);
}

bool is_generated_source_file(std::string_view file) {
    return file == "<string>";
}

bool contains_jni_identifier(std::string_view message) {
    if (message.find("JNIEnv") != std::string_view::npos ||
        message.find("JavaVM") != std::string_view::npos) {
        return true;
    }
    size_t start = 0;
    while ((start = message.find('\'', start)) != std::string_view::npos) {
        const size_t end = message.find('\'', start + 1);
        if (end == std::string_view::npos) {
            break;
        }
        if (looks_like_jni_identifier(message.substr(start + 1, end - start - 1))) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

std::string rewrite_location(
    const std::string &line,
    size_t preamble_lines) {
    const size_t first_colon = line.find(':');
    if (first_colon == std::string::npos) {
        return line;
    }
    const size_t second_colon = line.find(':', first_colon + 1);
    if (second_colon == std::string::npos) {
        return line;
    }
    const std::string_view file(line.data(), first_colon);
    const std::string_view line_number(
        line.data() + first_colon + 1,
        second_colon - first_colon - 1);
    size_t source_line = 0;
    if (!parse_decimal(line_number, &source_line)) {
        return line;
    }

    if (file == "hook.c" || !is_generated_source_file(file) ||
        source_line <= preamble_lines) {
        return line;
    }
    return "hook.c:" + std::to_string(source_line - preamble_lines) +
           line.substr(second_colon);
}

std::string normalize_locations(
    const std::string &message,
    size_t preamble_lines) {
    std::string normalized;
    size_t start = 0;
    while (start < message.size()) {
        size_t end = message.find('\n', start);
        const bool has_newline = end != std::string::npos;
        if (!has_newline) {
            end = message.size();
        }
        normalized.append(rewrite_location(message.substr(start, end - start), preamble_lines));
        if (has_newline) {
            normalized.push_back('\n');
            start = end + 1;
        } else {
            start = end;
        }
    }
    return normalized;
}

bool contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

void append_hint(std::vector<std::string> *hints, const char *hint) {
    if (hints != nullptr && hint != nullptr) {
        hints->emplace_back(hint);
    }
}

std::vector<std::string> collect_hints(std::string_view message) {
    std::vector<std::string> hints;
    if (contains(message, "parse error") ||
        contains(message, "expected") ||
        contains(message, "missing terminating")) {
        append_hint(
            &hints,
            "Hint: hook source is freestanding C; check for a missing ';', brace, quote, or C++-only syntax near the reported line.");
    }
    if (contains_jni_identifier(message)) {
        append_hint(
            &hints,
            "Hint: raw JNI headers and types are not available here; use opaque void * handles plus dejavu_hook_* helpers.");
    }
    if (contains(message, "implicit declaration of function") ||
        contains(message, "undefined symbol") ||
        contains(message, "undeclared")) {
        append_hint(
            &hints,
            "Hint: only the injected Hook C ABI helpers and Dejavu-registered symbols are available; check helper names and avoid unresolved libc/JNI calls.");
    }
    if ((contains(message, "before_hook") || contains(message, "after_hook")) &&
        (contains(message, "declaration") || contains(message, "arguments"))) {
        append_hint(
            &hints,
            "Hint: define callbacks as int before_hook(dejavu_hook_context *context) and/or int after_hook(dejavu_hook_context *context).");
    }
    return hints;
}

}  // namespace

size_t dejavu_hook_preamble_line_count(const char *preamble) {
    if (preamble == nullptr) {
        return 0;
    }
    size_t lines = 0;
    for (const char *cursor = preamble; *cursor != '\0'; ++cursor) {
        if (*cursor == '\n') {
            ++lines;
        }
    }
    return lines;
}

std::string dejavu_hook_format_compile_error(
    const std::string &message,
    const char *preamble) {
    const std::string normalized =
        normalize_locations(message, dejavu_hook_preamble_line_count(preamble));
    const std::vector<std::string> hints = collect_hints(normalized);
    if (hints.empty()) {
        return normalized;
    }

    std::string formatted = normalized;
    if (!formatted.empty() && formatted.back() != '\n') {
        formatted.push_back('\n');
    }
    for (const std::string &hint : hints) {
        formatted.append(hint);
        formatted.push_back('\n');
    }
    formatted.pop_back();
    return formatted;
}
