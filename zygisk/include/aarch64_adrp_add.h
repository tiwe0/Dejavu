#pragma once

#include <stddef.h>
#include <stdint.h>

uintptr_t dejavu_decode_aarch64_adrp_add_target(
    const uint32_t *instructions,
    size_t instruction_count,
    uintptr_t first_instruction_address);
