// Runs in an isolated process. Reserve virtual address space around a synthetic
// target, leaving either its last reachable page or one shared near page free.
#include <dobby.h>
#ifdef DEJAVU_CHECK_DIAGNOSTICS
#include <dobby/dejavu_hook_diagnostics.h>
#endif

#include <atomic>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <sys/mman.h>
#include <unistd.h>

namespace {
using Function = int (*)();
std::atomic<Function> original{nullptr};
std::atomic<unsigned> hits{0};

void check(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

__attribute__((noinline)) int replacement() {
    hits.fetch_add(1, std::memory_order_relaxed);
    auto function = original.load(std::memory_order_acquire);
    return function ? function() + 100 : -1;
}

void prepare(void *function, size_t patch_size, void *) {
    check(patch_size == 4, "entry patch must remain one instruction");
    original.store(reinterpret_cast<Function>(function), std::memory_order_release);
}

constexpr size_t kPoolTargets = 1025; // One more than a 16 KiB page's 16-byte slots.
struct PoolCall {
    std::atomic<Function> original{nullptr};
    std::atomic<unsigned> hits{0};
};
std::array<PoolCall, kPoolTargets> pool_calls;

template <size_t Index>
__attribute__((noinline)) int pool_replacement() {
    auto &call = pool_calls[Index];
    call.hits.fetch_add(1, std::memory_order_relaxed);
    auto function = call.original.load(std::memory_order_acquire);
    return function ? function() + 1000 + static_cast<int>(Index) : -1;
}

template <size_t... Index>
constexpr std::array<Function, sizeof...(Index)> pool_replacements(std::index_sequence<Index...>) {
    return {{&pool_replacement<Index>...}};
}
constexpr auto kPoolReplacements = pool_replacements(std::make_index_sequence<kPoolTargets>{});

void pool_prepare(void *function, size_t patch_size, void *opaque) {
    check(patch_size == 4, "pooled entry patch must remain one instruction");
    static_cast<PoolCall *>(opaque)->original.store(reinterpret_cast<Function>(function),
                                                   std::memory_order_release);
}

uintptr_t branch_destination(uint32_t *entry) {
    const uint32_t instruction = __atomic_load_n(entry, __ATOMIC_ACQUIRE);
    check((instruction & 0xfc000000U) == 0x14000000U, "entry must contain an ARM64 B");
    int64_t immediate = instruction & 0x03ffffffU;
    if (immediate & (int64_t{1} << 25))
        immediate -= int64_t{1} << 26;
    const auto address = reinterpret_cast<uintptr_t>(entry);
    return immediate < 0 ? address - static_cast<uintptr_t>(-immediate) * 4
                         : address + static_cast<uintptr_t>(immediate) * 4;
}

void check_near_failure(int status, uint32_t *target, size_t index) {
    check(status != 0, "expected exhausted near-page search to fail");
    check(pool_calls[index].original.load() == nullptr, "failed install must not publish original");
    check(DobbyIsHooked(target) == 0, "failed install must not register a hook");
    check(reinterpret_cast<Function>(target)() == 42 + static_cast<int>(index),
          "failed install must preserve target instructions");
#ifdef DEJAVU_CHECK_DIAGNOSTICS
    const char *stage = dejavu_dobby::LastHookFailureStage();
    check(stage && std::strcmp(stage, "near-allocation") == 0,
          "exhausted pool must report near-allocation");
#endif
}

int run_pool_test(bool baseline, bool full) {
    constexpr uintptr_t range = uintptr_t{1} << 27;
    constexpr size_t slot_size = 16;
    const long runtime_page = sysconf(_SC_PAGESIZE);
    check(runtime_page == 4096 || runtime_page == 16384, "pool test requires a 4K or 16K page");
    const auto page = static_cast<size_t>(runtime_page);
    const size_t capacity = page / slot_size;
    const size_t first_count = full ? capacity : 64;
    const size_t total = full ? capacity + 1 : 80;
    check(total <= kPoolTargets && 4 + total * 8 <= page, "synthetic targets must fit one page");
    void *reservation = mmap(nullptr, 2 * range + 2 * page, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    check(reservation != MAP_FAILED, "pool virtual reservation failed");
    const auto base = reinterpret_cast<uintptr_t>(reservation);
    void *target_page = reinterpret_cast<void *>(base + range);
    const auto near_page = base + range + 4 * page;
    check(mprotect(target_page, page, PROT_READ | PROT_WRITE) == 0, "pool targets writable");
    std::array<uint32_t *, kPoolTargets> targets{};
    for (size_t index = 0; index < total; ++index) {
        auto *entry = reinterpret_cast<uint32_t *>(base + range + 4 + index * 8);
        entry[0] = 0x52800000U | (static_cast<uint32_t>(42 + index) << 5); // mov w0, #value
        entry[1] = 0xd65f03c0; // ret
        targets[index] = entry;
        const auto destination = reinterpret_cast<uintptr_t>(kPoolReplacements[index]);
        const auto source = reinterpret_cast<uintptr_t>(entry);
        check(source % 4 == 0 && destination % 4 == 0, "pool entry and replacement must be aligned");
        check(destination > source ? destination - source >= range : source - destination > range,
              "every replacement must require a near trampoline");
    }
    __builtin___clear_cache(reinterpret_cast<char *>(target_page),
                            reinterpret_cast<char *>(target_page) + page);
    check(mprotect(target_page, page, PROT_READ | PROT_EXEC) == 0, "pool targets executable");
    check(munmap(reinterpret_cast<void *>(near_page), page) == 0, "release unique near page");

    std::array<uintptr_t, kPoolTargets> slots{};
    std::array<std::array<unsigned char, slot_size>, kPoolTargets> saved_slots{};
    auto verify_slots = [&](size_t count) {
        for (size_t previous = 0; previous < count; ++previous)
            check(std::memcmp(reinterpret_cast<void *>(slots[previous]), saved_slots[previous].data(),
                              slot_size) == 0, "adding a hook must preserve every previous slot");
    };
    auto install = [&](size_t index) {
        const int status = DobbyHookWithPrepare(targets[index],
                                                reinterpret_cast<void *>(kPoolReplacements[index]),
                                                pool_prepare, &pool_calls[index]);
        if (baseline && index == 1) {
            check_near_failure(status, targets[index], index);
            return false;
        }
        check(status == 0, "shared unique near page must support another hook");
        const auto slot = branch_destination(targets[index]);
        check(slot >= near_page && slot - near_page <= page - slot_size,
              "every forward trampoline must belong to the unique near page");
        check(slot % slot_size == 0, "forward slots must remain 16-byte aligned");
        for (size_t previous = 0; previous < index; ++previous)
            check(slot != slots[previous], "published slots must never be reused");
        slots[index] = slot;
        verify_slots(index);
        std::memcpy(saved_slots[index].data(), reinterpret_cast<void *>(slot), slot_size);
        check(pool_calls[index].original.load() && pool_calls[index].original.load()() ==
                  42 + static_cast<int>(index), "each pooled original must remain callable");
        check(reinterpret_cast<Function>(targets[index])() == 1042 + 2 * static_cast<int>(index),
              "each pooled replacement must call its own original");
        check(pool_calls[index].hits.load() == 1, "each pooled replacement must record a hit");
        return true;
    };

    for (size_t index = 0; index < first_count; ++index) {
        if (!install(index)) {
            check(DobbyDestroy(targets[0]) == 0, "baseline first hook uninstall");
            std::puts("PASS: baseline consumes unique page; second hook fails without a pool");
            return 0;
        }
    }
    if (full) {
        const int status = DobbyHookWithPrepare(targets[capacity],
            reinterpret_cast<void *>(kPoolReplacements[capacity]), pool_prepare, &pool_calls[capacity]);
        check_near_failure(status, targets[capacity], capacity);
        verify_slots(capacity);
        for (size_t index = 0; index < capacity; ++index) {
            check(DobbyDestroy(targets[index]) == 0, "full pool uninstall");
            check(pool_calls[index].original.load()() == 42 + static_cast<int>(index),
                  "full pool original survives uninstall");
        }
        std::printf("PASS: unique %zu-byte page holds %zu hooks; full pool fails safely\n", page, capacity);
        return 0;
    }

    // Retain references that model a caller already inside a published forward
    // trampoline. Uninstalling the entry must not make their slots reusable.
    for (size_t index = 0; index < 16; ++index) {
        check(DobbyDestroy(targets[index]) == 0, "pooled early uninstall");
        check(reinterpret_cast<Function>(targets[index])() == 42 + static_cast<int>(index),
              "uninstalled pooled entry must be restored");
    }
    for (size_t index = first_count; index < total; ++index)
        check(install(index), "pool must append after uninstall with no new mapping gap");
    verify_slots(total);
    for (size_t index = 0; index < 16; ++index) {
        check(pool_calls[index].original.load()() == 42 + static_cast<int>(index),
              "saved original survives uninstall and later installs");
        check(reinterpret_cast<Function>(slots[index])() == 1042 + 2 * static_cast<int>(index),
              "published forward slot survives uninstall and later installs");
        check(pool_calls[index].hits.load() == 2, "saved forward still uses its original callback");
    }
    for (size_t index = 16; index < total; ++index)
        check(DobbyDestroy(targets[index]) == 0, "remaining pooled hook uninstall");
    for (size_t index = 0; index < total; ++index)
        check(reinterpret_cast<Function>(targets[index])() == 42 + static_cast<int>(index),
              "all pooled target entries restored");
    std::puts("PASS: 80 hooks share one near page; originals and published slots survive uninstall");
    return 0;
}
}

int main(int argc, char **argv) {
    const bool baseline = argc == 2 && std::strcmp(argv[1], "--expect-failure") == 0;
    const bool exhausted = argc == 2 && std::strcmp(argv[1], "--exhausted") == 0;
    const bool pool = argc == 2 && std::strcmp(argv[1], "--pool") == 0;
    const bool pool_baseline = argc == 2 && std::strcmp(argv[1], "--pool-expect-failure") == 0;
    const bool pool_full = argc == 2 && std::strcmp(argv[1], "--pool-full") == 0;
    check(argc == 1 || baseline || exhausted || pool || pool_baseline || pool_full,
          "unsupported argument");
    if (pool || pool_baseline || pool_full)
        return run_pool_test(pool_baseline, pool_full);
    constexpr uintptr_t range = uintptr_t{1} << 27;
    const long page_size = sysconf(_SC_PAGESIZE);
    check(page_size > 0, "page size unavailable");
    const auto page = static_cast<size_t>(page_size);
    const size_t reservation_size = 2 * range + 2 * page;
    void *reservation = mmap(nullptr, reservation_size, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    check(reservation != MAP_FAILED, "virtual reservation failed");
    const auto base = reinterpret_cast<uintptr_t>(reservation);
    void *entry_page = reinterpret_cast<void *>(base + range);
    auto *instructions = reinterpret_cast<uint32_t *>(base + range + 4);
    check(mprotect(entry_page, page, PROT_READ | PROT_WRITE) == 0, "target writable");
    instructions[0] = 0x52800540; // mov w0, #42
    instructions[1] = 0xd65f03c0; // ret
    __builtin___clear_cache(reinterpret_cast<char *>(instructions),
                            reinterpret_cast<char *>(instructions + 2));
    check(mprotect(entry_page, page, PROT_READ | PROT_EXEC) == 0, "target executable");
    auto target = reinterpret_cast<Function>(instructions);
    check(target() == 42, "original target result");
    const auto from = reinterpret_cast<uintptr_t>(instructions);
    const auto to = reinterpret_cast<uintptr_t>(&replacement);
    check(to > from ? to - from >= range : from - to > range,
          "replacement must require a near trampoline");
    if (!exhausted)
        check(munmap(reinterpret_cast<void *>(base + 2 * range), page) == 0,
              "release final reachable page");

    const int status = DobbyHookWithPrepare(reinterpret_cast<void *>(instructions),
                                           reinterpret_cast<void *>(&replacement), prepare, nullptr);
    if (baseline || exhausted) {
        check(status != 0, "expected conservative install failure");
        check(original.load() == nullptr, "failed search must not publish original");
        check(DobbyIsHooked(reinterpret_cast<void *>(instructions)) == 0, "no hook registered");
        check(target() == 42 && hits.load() == 0, "failed search leaves target untouched");
#ifdef DEJAVU_CHECK_DIAGNOSTICS
        const char *stage = dejavu_dobby::LastHookFailureStage();
        check(stage && std::strcmp(stage, "near-allocation") == 0,
              "failure must identify near allocation");
#endif
        std::puts("PASS: failed near search preserves entry and original publication");
        return 0;
    }
    check(status == 0, "final reachable page must support installation");
    check(original.load() && original.load()() == 42, "original trampoline callable");
    check(target() == 142 && hits.load() == 1, "replacement and hit count");
    check(DobbyDestroy(reinterpret_cast<void *>(instructions)) == 0, "uninstall");
    check(target() == 42, "uninstall restores original target");
#ifdef DEJAVU_CHECK_DIAGNOSTICS
    check(dejavu_dobby::LastHookFailureStage() == nullptr, "success clears failure stage");
#endif
    std::puts("PASS: final reachable page, four-byte entry, original, hits and uninstall");
    return 0;
}
