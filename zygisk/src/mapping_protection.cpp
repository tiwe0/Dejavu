#include "mapping_protection.h"

#include <sys/mman.h>

#include <algorithm>
#include <cstdio>
#include <limits>
#include <sstream>
#include <utility>

namespace {

bool valid_snapshot(const RootHelperProtectionSnapshot &snapshot, std::string *error) {
    uintptr_t previous_end = 0;
    if (snapshot.ranges.empty()) {
        *error = "empty mapping protection snapshot";
        return false;
    }
    for (const auto &range : snapshot.ranges) {
        if (range.length == 0 ||
            range.start > std::numeric_limits<uintptr_t>::max() - range.length ||
            (range.protection & PROT_EXEC) == 0 || range.start < previous_end) {
            *error = "invalid mapping protection snapshot";
            return false;
        }
        previous_end = range.start + range.length;
    }
    return true;
}

std::string range_failure(const RootHelperProtectionRange &range, const std::string &detail) {
    std::ostringstream message;
    message << "0x" << std::hex << range.start << "+0x" << range.length << ": "
            << (detail.empty() ? "unable to change mapping protection" : detail);
    return message.str();
}

} // namespace

bool capture_mapping_protection(std::istream &maps, uintptr_t address, size_t size,
                                size_t page_size, RootHelperProtectionSnapshot *snapshot,
                                std::string *error) {
    snapshot->ranges.clear();
    error->clear();
    const uintptr_t maximum = std::numeric_limits<uintptr_t>::max();
    if (address == 0 || size == 0 || page_size == 0 || address > maximum - size) {
        *error = "invalid target protection span";
        return false;
    }
    const uintptr_t start = address - address % page_size;
    uintptr_t end = address + size;
    const size_t remainder = end % page_size;
    if (remainder != 0) {
        const size_t padding = page_size - remainder;
        if (end > maximum - padding) {
            *error = "target protection span overflows a page boundary";
            return false;
        }
        end += padding;
    }
    uintptr_t covered = start;
    RootHelperProtectionSnapshot captured;
    std::string line;
    while (std::getline(maps, line) && covered < end) {
        unsigned long long map_start = 0;
        unsigned long long map_end = 0;
        char permissions[8] = {};
        if (std::sscanf(line.c_str(), "%llx-%llx %7s", &map_start, &map_end, permissions) != 3 ||
            map_start >= map_end || map_end > maximum) {
            continue;
        }
        if (map_end <= covered || map_start >= end) {
            continue;
        }
        if (map_start > covered) {
            *error = "target protection span contains an unmapped gap";
            return false;
        }
        if (permissions[2] != 'x') {
            *error = "target protection span contains a non-executable mapping";
            return false;
        }
        const uintptr_t range_end = std::min<uintptr_t>(map_end, end);
        if (covered % page_size != 0 || range_end % page_size != 0) {
            *error = "target mapping boundaries are not page aligned";
            return false;
        }
        int protection = PROT_EXEC;
        if (permissions[0] == 'r')
            protection |= PROT_READ;
        if (permissions[1] == 'w')
            protection |= PROT_WRITE;
        const size_t length = range_end - covered;
        if (!captured.ranges.empty() && captured.ranges.back().protection == protection &&
            captured.ranges.back().start + captured.ranges.back().length == covered) {
            captured.ranges.back().length += length;
        } else {
            captured.ranges.push_back({covered, length, protection});
        }
        covered = range_end;
    }
    if (covered != end) {
        *error = "target protection span is not fully mapped";
        return false;
    }
    *snapshot = std::move(captured);
    return true;
}

bool apply_mapping_read_write(const RootHelperProtectionSnapshot &snapshot,
                              const MappingProtectionWriter &write, std::string *error) {
    error->clear();
    if (!valid_snapshot(snapshot, error))
        return false;
    for (size_t index = 0; index < snapshot.ranges.size(); ++index) {
        const auto &range = snapshot.ranges[index];
        std::string detail;
        if (write(range, range.protection | PROT_READ | PROT_WRITE, &detail))
            continue;
        *error = "unable to make target writable: " + range_failure(range, detail);
        // A lost RPC reply can mean the failing range was already changed.
        // Include it in rollback rather than assuming failure means no effect.
        for (size_t undo = index + 1; undo > 0; --undo) {
            const auto &original = snapshot.ranges[undo - 1];
            std::string restore_error;
            if (!write(original, original.protection, &restore_error)) {
                *error += "; rollback failed: " + range_failure(original, restore_error);
            }
        }
        return false;
    }
    return true;
}

bool restore_mapping_protection(const RootHelperProtectionSnapshot &snapshot,
                                const MappingProtectionWriter &write, std::string *error) {
    error->clear();
    if (!valid_snapshot(snapshot, error))
        return false;
    bool restored = true;
    for (const auto &range : snapshot.ranges) {
        std::string detail;
        if (!write(range, range.protection, &detail)) {
            if (!error->empty())
                *error += "; ";
            *error += "unable to restore target protection: " + range_failure(range, detail);
            restored = false;
        }
    }
    return restored;
}
