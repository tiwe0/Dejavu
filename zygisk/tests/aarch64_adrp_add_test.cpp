#include "aarch64_adrp_add.h"

#include <assert.h>
#include <stdint.h>

int main() {
    constexpr uintptr_t kLoadBias = uintptr_t{0x7000000000};
    constexpr uintptr_t kFunction = kLoadBias + 0x4bbcb4;
    const uint32_t android_15_oracle[] = {
        0xd503245fU,
        0xf940b808U,
        0x90ffeb69U,
        0x91010129U,
        0xeb09003fU,
        0xfa411104U,
        0x1a9f17e0U,
        0xd65f03c0U,
    };
    assert(dejavu_decode_aarch64_adrp_add_target(
               android_15_oracle,
               sizeof(android_15_oracle) / sizeof(android_15_oracle[0]),
               kFunction) == kLoadBias + 0x227040);

    const uint32_t mismatched_registers[] = {0x90000009U, 0x91000128U};
    assert(dejavu_decode_aarch64_adrp_add_target(
               mismatched_registers,
               sizeof(mismatched_registers) / sizeof(mismatched_registers[0]),
               kFunction) == 0);
    assert(dejavu_decode_aarch64_adrp_add_target(nullptr, 0, kFunction) == 0);
    return 0;
}
