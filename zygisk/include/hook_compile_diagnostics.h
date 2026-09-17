#pragma once

#include <cstddef>
#include <string>

size_t dejavu_hook_preamble_line_count(const char *preamble);
std::string dejavu_hook_format_compile_error(
    const std::string &message,
    const char *preamble);
