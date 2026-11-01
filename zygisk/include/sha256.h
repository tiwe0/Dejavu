#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

std::string sha256_hex(const void *data, size_t size);
bool sha256_file_hex(const std::string &path, std::string *digest, std::string *error);
