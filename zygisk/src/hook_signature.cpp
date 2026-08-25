#include "hook_signature.h"

#include <cstddef>

namespace {

bool parse_type(
    const std::string &signature,
    size_t *offset,
    bool allow_void,
    char *kind) {
    if (*offset >= signature.size()) {
        return false;
    }
    const char descriptor = signature[(*offset)++];
    switch (descriptor) {
        case 'Z':
        case 'B':
        case 'C':
        case 'S':
        case 'I':
        case 'J':
        case 'F':
        case 'D':
            *kind = descriptor;
            return true;
        case 'V':
            if (allow_void) {
                *kind = descriptor;
                return true;
            }
            return false;
        case 'L': {
            const size_t end = signature.find(';', *offset);
            if (end == std::string::npos || end == *offset) {
                return false;
            }
            *offset = end + 1;
            *kind = 'L';
            return true;
        }
        case '[': {
            while (*offset < signature.size() && signature[*offset] == '[') {
                ++*offset;
            }
            char element = '\0';
            if (!parse_type(signature, offset, false, &element)) {
                return false;
            }
            *kind = 'L';
            return true;
        }
        default:
            return false;
    }
}

}  // namespace

bool dejavu_parse_hook_signature(
    const std::string &signature,
    DejavuHookSignature *parsed,
    std::string *error) {
    if (parsed == nullptr || signature.empty() || signature[0] != '(') {
        if (error != nullptr) {
            *error = "invalid JNI method signature";
        }
        return false;
    }

    DejavuHookSignature result;
    size_t offset = 1;
    while (offset < signature.size() && signature[offset] != ')') {
        char kind = '\0';
        if (!parse_type(signature, &offset, false, &kind)) {
            if (error != nullptr) {
                *error = "invalid JNI parameter descriptor";
            }
            return false;
        }
        result.parameters.push_back(kind);
    }
    if (offset >= signature.size() || signature[offset++] != ')' ||
        !parse_type(signature, &offset, true, &result.result) ||
        offset != signature.size()) {
        if (error != nullptr) {
            *error = "invalid JNI return descriptor";
        }
        return false;
    }
    *parsed = std::move(result);
    return true;
}
