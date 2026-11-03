#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <istream>
#include <string>
#include <vector>

struct RootHelperProtectionRange {
    uintptr_t start = 0;
    size_t length = 0;
    int protection = 0;
};

struct RootHelperProtectionSnapshot {
    std::vector<RootHelperProtectionRange> ranges;
};

using MappingProtectionWriter =
    std::function<bool(const RootHelperProtectionRange &, int, std::string *)>;

// Capture only pages covering the supplied byte span, before permissions change.
// Every captured page must be executable; mixed original permissions are retained.
bool capture_mapping_protection(std::istream &maps, uintptr_t address, size_t size,
                                size_t page_size, RootHelperProtectionSnapshot *snapshot,
                                std::string *error);

// If applying a range fails, attempt to restore it and all preceding ranges.
// The snapshot remains intact so the caller can retry a failed restoration.
bool apply_mapping_read_write(const RootHelperProtectionSnapshot &snapshot,
                              const MappingProtectionWriter &write, std::string *error);
bool restore_mapping_protection(const RootHelperProtectionSnapshot &snapshot,
                                const MappingProtectionWriter &write, std::string *error);
