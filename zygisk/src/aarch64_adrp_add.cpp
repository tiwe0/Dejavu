#include "aarch64_adrp_add.h"

namespace {

int64_t sign_extend_21(uint64_t value) {
    constexpr uint64_t kSignBit = uint64_t{1} << 20;
    return static_cast<int64_t>((value ^ kSignBit) - kSignBit);
}

}  // namespace

uintptr_t dejavu_decode_aarch64_adrp_add_target(
    const uint32_t *instructions,
    size_t instruction_count,
    uintptr_t first_instruction_address) {
    if (instructions == nullptr || instruction_count < 2) {
        return 0;
    }

    for (size_t index = 0; index + 1 < instruction_count; ++index) {
        const uint32_t adrp = instructions[index];
        const uint32_t add = instructions[index + 1];
        if ((adrp & 0x9f000000U) != 0x90000000U ||
            (add & 0xff000000U) != 0x91000000U) {
            continue;
        }

        const uint32_t adrp_destination = adrp & 0x1fU;
        const uint32_t add_source = (add >> 5) & 0x1fU;
        const uint32_t add_destination = add & 0x1fU;
        if (adrp_destination != add_source || add_source != add_destination) {
            continue;
        }

        const uint64_t immediate_pages =
            (static_cast<uint64_t>((adrp >> 5) & 0x7ffffU) << 2) |
            static_cast<uint64_t>((adrp >> 29) & 0x3U);
        const int64_t page_delta = sign_extend_21(immediate_pages) * 4096;
        const uintptr_t instruction_address =
            first_instruction_address + index * sizeof(uint32_t);
        const uintptr_t page = instruction_address & ~uintptr_t{0xfff};
        const uint64_t add_immediate = static_cast<uint64_t>((add >> 10) & 0xfffU)
            << (((add >> 22) & 0x1U) != 0 ? 12 : 0);
        return static_cast<uintptr_t>(
            static_cast<intptr_t>(page) + page_delta + static_cast<int64_t>(add_immediate));
    }
    return 0;
}
