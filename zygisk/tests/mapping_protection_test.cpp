#include "mapping_protection.h"

#include <sys/mman.h>

#include <cassert>
#include <cstdio>
#include <limits>
#include <map>
#include <sstream>
#include <tuple>
#include <vector>

namespace {

RootHelperProtectionSnapshot capture(const char *text, uintptr_t address, size_t size,
                                     size_t page_size = 0x1000) {
    std::istringstream maps(text);
    RootHelperProtectionSnapshot snapshot;
    std::string error;
    assert(capture_mapping_protection(maps, address, size, page_size, &snapshot, &error));
    assert(error.empty());
    return snapshot;
}

bool rejects(const char *text, uintptr_t address, size_t size, size_t page_size = 0x1000) {
    std::istringstream maps(text);
    RootHelperProtectionSnapshot snapshot{{{0x1000, 0x1000, PROT_EXEC}}};
    std::string error;
    const bool accepted =
        capture_mapping_protection(maps, address, size, page_size, &snapshot, &error);
    assert(!accepted && !error.empty() && snapshot.ranges.empty());
    return !accepted;
}

void test_minimal_saved_range_survives_vma_split() {
    const auto snapshot = capture("1000-9000 --xp 00000000 00:00 1 /libc.so\n"
                                  "9000-a000 rw-p 00008000 00:00 1 /libc.so\n",
                                  0x5120, 16);
    assert(snapshot.ranges.size() == 1);
    const auto &range = snapshot.ranges[0];
    assert(range.start == 0x5000 && range.length == 0x1000 && range.protection == PROT_EXEC);
    // After patching, a fresh maps lookup could report only a small RX VMA.
    // Restoring must not combine that new start with the old 0x8000 text size.
    const auto split = capture("1000-5000 rwxp 00000000 00:00 1 /libc.so\n"
                               "5000-6000 r-xp 00004000 00:00 1 /libc.so\n"
                               "6000-9000 rwxp 00005000 00:00 1 /libc.so\n"
                               "9000-a000 rw-p 00008000 00:00 1 /libc.so\n",
                               0x5120, 16);
    assert(split.ranges[0].protection == (PROT_READ | PROT_EXEC));
    std::vector<std::tuple<uintptr_t, size_t, int>> writes;
    std::string error;
    assert(restore_mapping_protection(
        snapshot,
        [&](const RootHelperProtectionRange &saved, int protection, std::string *) {
            writes.emplace_back(saved.start, saved.length, protection);
            return true;
        },
        &error));
    assert(writes.size() == 1);
    assert(writes[0] == std::make_tuple(uintptr_t{0x5000}, size_t{0x1000}, PROT_EXEC));
}

void test_cross_page_and_runtime_page_size() {
    const auto mixed = capture("1000-2000 r-xp 0 00:00 1 /first.so\n"
                               "2000-3000 --xp 0 00:00 2 /second.so\n",
                               0x1ff8, 16);
    assert(mixed.ranges.size() == 2);
    assert(mixed.ranges[0].start == 0x1000 && mixed.ranges[0].length == 0x1000);
    assert(mixed.ranges[0].protection == (PROT_READ | PROT_EXEC));
    assert(mixed.ranges[1].start == 0x2000 && mixed.ranges[1].length == 0x1000);
    assert(mixed.ranges[1].protection == PROT_EXEC);

    const auto large_pages = capture("4000-14000 --xp 0 00:00 1 /text.so\n", 0xbff8, 16, 0x4000);
    assert(large_pages.ranges.size() == 1);
    assert(large_pages.ranges[0].start == 0x8000 && large_pages.ranges[0].length == 0x8000);

    const auto merged = capture("1000-2000 r-xp 0 00:00 1 /first.so\n"
                                "2000-3000 r-xp 0 00:00 2 /second.so\n",
                                0x1ff8, 16);
    assert(merged.ranges.size() == 1 && merged.ranges[0].length == 0x2000);
}

void test_invalid_spans_do_not_publish_a_partial_snapshot() {
    assert(rejects("1000-2000 --xp 0 00:00 1 /text.so\n"
                   "2000-3000 rw-p 0 00:00 1 /text.so\n",
                   0x1ff8, 16));
    assert(rejects("1000-2000 r-xp 0 00:00 1 /text.so\n"
                   "3000-4000 r-xp 0 00:00 1 /text.so\n",
                   0x1ff8, 0x1010));
    assert(rejects("1000-2000 r-xp 0 00:00 1 /text.so\n", 0x1ff8, 16));
    assert(rejects("", std::numeric_limits<uintptr_t>::max() - 7, 16));
    assert(rejects("", std::numeric_limits<uintptr_t>::max() - 7, 4));
    assert(rejects("", 0, 16));
    assert(rejects("", 0x1000, 0));
    assert(rejects("", 0x1000, 16, 0));
    assert(rejects("1001-2000 r-xp 0 00:00 1 /text.so\n", 0x1100, 16));
}

void test_apply_failure_restores_already_changed_and_uncertain_range() {
    const auto snapshot = capture("1000-2000 r-xp 0 00:00 1 /text.so\n"
                                  "2000-3000 --xp 0 00:00 1 /text.so\n",
                                  0x1ff8, 16);
    std::map<uintptr_t, int> protections{{0x1000, PROT_READ | PROT_EXEC}, {0x2000, PROT_EXEC}};
    std::vector<std::tuple<uintptr_t, int>> writes;
    bool failed_once = false;
    std::string error;
    assert(!apply_mapping_read_write(
        snapshot,
        [&](const RootHelperProtectionRange &range, int protection, std::string *detail) {
            writes.emplace_back(range.start, protection);
            protections[range.start] = protection;
            if (range.start == 0x2000 && !failed_once) {
                failed_once = true;
                *detail = "RPC reply was lost after mprotect";
                return false;
            }
            return true;
        },
        &error));
    assert(error.find("RPC reply was lost") != std::string::npos);
    assert(error.find("rollback failed") == std::string::npos);
    assert(writes.size() == 4 && std::get<0>(writes[2]) == 0x2000 &&
           std::get<0>(writes[3]) == 0x1000);
    assert(protections[0x1000] == (PROT_READ | PROT_EXEC) && protections[0x2000] == PROT_EXEC);
    assert(snapshot.ranges.size() == 2);
}

void test_failed_rollback_and_restore_remain_retryable() {
    const auto snapshot = capture("1000-2000 r-xp 0 00:00 1 /text.so\n"
                                  "2000-3000 --xp 0 00:00 1 /text.so\n",
                                  0x1ff8, 16);
    int writes = 0;
    std::string error;
    assert(!apply_mapping_read_write(
        snapshot,
        [&](const RootHelperProtectionRange &, int, std::string *detail) {
            ++writes;
            if (writes == 2 || writes == 3) {
                *detail = "injected failure";
                return false;
            }
            return true;
        },
        &error));
    assert(writes == 4 && error.find("rollback failed") != std::string::npos);
    writes = 0;
    assert(!restore_mapping_protection(
        snapshot,
        [&](const RootHelperProtectionRange &, int, std::string *detail) {
            if (++writes == 1) {
                *detail = "temporary restore failure";
                return false;
            }
            return true;
        },
        &error));
    assert(writes == 2 && error.find("temporary restore failure") != std::string::npos);
    assert(restore_mapping_protection(
        snapshot, [](const RootHelperProtectionRange &, int, std::string *) { return true; },
        &error));
    assert(error.empty());
}

} // namespace

int main() {
    test_minimal_saved_range_survives_vma_split();
    test_cross_page_and_runtime_page_size();
    test_invalid_spans_do_not_publish_a_partial_snapshot();
    test_apply_failure_restores_already_changed_and_uncertain_range();
    test_failed_rollback_and_restore_remain_retryable();
    std::puts(
        "OK: exact protection snapshots survive VMA splits, cross-page spans and failed rollback");
}
