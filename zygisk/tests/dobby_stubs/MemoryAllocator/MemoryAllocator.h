#pragma once
#include <dobby/common.h>
struct MemBlock {
    uintptr_t address = 0;
    size_t size = 0;
    MemBlock() = default;
    MemBlock(uintptr_t address, size_t size) : address(address), size(size) {
    }
    uintptr_t addr() const {
        return address;
    }
};
