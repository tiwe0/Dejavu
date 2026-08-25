#pragma once

#include <string>
#include <vector>

struct DejavuHookSignature {
    std::vector<char> parameters;
    char result = '\0';
};

bool dejavu_parse_hook_signature(
    const std::string &signature,
    DejavuHookSignature *parsed,
    std::string *error);
