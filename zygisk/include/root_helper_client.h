#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

/*
 * Prototype client for the root helper. The helper runs as root outside the
 * app and injects mprotect into this process via ptrace so execute-only text
 * can be temporarily made readable/writable for Dobby.
 */
bool root_helper_available();
bool root_helper_mapping(void *address, uintptr_t *start, size_t *length, int *protection,
                         std::string *error);
bool root_helper_unlock(void *address, int *original_protection, size_t *mapping_length,
                        std::string *error);
bool root_helper_restore(void *address, size_t mapping_length, int protection, std::string *error);
