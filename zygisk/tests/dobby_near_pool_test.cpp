#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <vector>
#include <algorithm>
#include <sys/mman.h>
#include <unistd.h>
#include <dobby/common.h>
#include <dobby/dejavu_hook_diagnostics.h>
#include DOBBY_REAL_TRAMPOLINE

#define ARM64_B_XXX_RANGE (1ull << 27)
struct Region {
    uintptr_t begin, finish;
    uintptr_t addr() const {
        return begin;
    }
    uintptr_t end() const {
        return finish;
    }
};
static std::vector<Region> regions;
struct ProcessRuntime {
    static const std::vector<Region> &getMemoryLayout() {
        return regions;
    }
};
static void (*test_os_observer)() = nullptr;
static long test_page_size = 4096;
static unsigned maps = 0, unmaps = 0, restores = 0;
static bool fail_restore = false, fail_unmap = false;
static int patch_status = 0;
static uintptr_t last_patch = 0;
static size_t last_patch_size = 0;
static size_t forward_extra_bytes = 0;
static long TestSysconf(int name) {
    assert(name == _SC_PAGESIZE);
    return test_page_size;
}
static void *TestMmap(void *hint, size_t size, int protection, int flags, int fd, int offset) {
    assert(size == static_cast<size_t>(test_page_size));
    assert(protection == (PROT_READ | PROT_EXEC));
    assert((flags & MAP_ANONYMOUS) && (flags & MAP_PRIVATE) && !(flags & MAP_FIXED));
    assert(fd == -1 && offset == 0);
    if (test_os_observer)
        test_os_observer();
    ++maps;
    auto address = reinterpret_cast<uintptr_t>(hint);
    regions.push_back({address, address + size});
    std::sort(regions.begin(), regions.end(),
              [](const auto &a, const auto &b) { return a.begin < b.begin; });
    return hint;
}
static int TestMunmap(void *address, size_t size) {
    if (test_os_observer)
        test_os_observer();
    ++unmaps;
    if (fail_unmap) {
        errno = EBUSY;
        return -1;
    }
    auto start = reinterpret_cast<uintptr_t>(address);
    regions.erase(std::remove_if(regions.begin(), regions.end(),
                                 [&](const auto &region) {
                                     return region.begin == start && region.finish == start + size;
                                 }),
                  regions.end());
    return 0;
}
static int TestMprotect(void *, size_t size, int protection) {
    assert(size == static_cast<size_t>(test_page_size));
    assert(protection == (PROT_READ | PROT_EXEC));
    if (test_os_observer)
        test_os_observer();
    ++restores;
    if (fail_restore) {
        errno = EACCES;
        return -1;
    }
    return 0;
}
int DobbyCodePatch(void *address, uint8_t *, uint32_t size) {
    if (test_os_observer)
        test_os_observer();
    last_patch = reinterpret_cast<uintptr_t>(address);
    last_patch_size = size;
    return patch_status;
}
struct FakeCodeBuffer {
    uint8_t bytes[16]{};
    size_t length = 0;
    uint8_t *data() {
        return bytes;
    }
    size_t size() const {
        return length;
    }
    MemBlock dup() {
        return {0xdead0000, length};
    }
};
struct TurboAssembler {
    FakeCodeBuffer buffer;
    explicit TurboAssembler(addr_t = 0) {
    }
    void ldr(int, int) {
        buffer.length += 4;
    }
    void br(int) {
        buffer.length += 4;
    }
    void EmitInt64(uint64_t) {
        buffer.length += 8 + forward_extra_bytes;
    }
    void b(addr_t) {
        buffer.length += 4;
    }
    FakeCodeBuffer *code_buffer() {
        return &buffer;
    }
};
static constexpr int TMP_REG_0 = 17;
#define mmap TestMmap
#define munmap TestMunmap
#define mprotect TestMprotect
#define sysconf TestSysconf
#include DOBBY_SAFE_NEAR_HELPERS
#include DOBBY_SAFE_NEAR_POOL
static SafeNearPagePool gSafeNearPagePool;
#if !defined(__linux__)
#define __linux__ 1
#endif
#include DOBBY_SAFE_NEAR_ROUTING
#undef mmap
#undef munmap
#undef mprotect
#undef sysconf
struct TestRouting {
    Trampoline *trampoline = nullptr;
    Trampoline *near_trampoline = nullptr;
    bool require_atomic_patch = true;
#include DOBBY_SAFE_NEAR_DESTRUCTOR
};

static constexpr uintptr_t source = 0x100000000;
static constexpr uintptr_t replacement = 0x200000000;
static void Reset() {
    gSafeNearPagePool.pages.clear();
    maps = unmaps = restores = 0;
    fail_restore = fail_unmap = false;
    patch_status = 0;
    forward_extra_bytes = 0;
    const auto lower = source - ARM64_B_XXX_RANGE;
    const auto upper = source + ARM64_B_XXX_RANGE;
    regions = {{lower + static_cast<uintptr_t>(test_page_size), upper}};
}
static void Delete(Trampoline *trampoline) {
    ReleaseSafeNearTrampoline(trampoline);
    delete trampoline;
}
static void TestSharingAndCapacity() {
    Reset();
    const size_t capacity = test_page_size / 16;
    std::vector<Trampoline *> trampolines;
    for (size_t index = 0; index < capacity; ++index) {
        auto trampoline = GenerateSafeNearTrampolineBuffer(source, replacement);
        assert(trampoline && trampoline->size() == 4 && last_patch_size == 16);
        assert(trampoline->strict_near_slot == source - ARM64_B_XXX_RANGE + index * 16);
        PublishSafeNearTrampoline(trampoline);
        trampolines.push_back(trampoline);
    }
    assert(maps == 1);
    assert(!GenerateSafeNearTrampolineBuffer(source, replacement));
    assert(std::strcmp(dejavu_dobby::LastHookFailureStage(), "near-allocation") == 0);
    for (auto trampoline : trampolines)
        Delete(trampoline);
    assert(unmaps == 0 && gSafeNearPagePool.pages.size() == 1);
    assert(!GenerateSafeNearTrampolineBuffer(source, replacement));
}
static void TestRollback() {
    Reset();
    {
        TestRouting routing;
        routing.near_trampoline = GenerateSafeNearTrampolineBuffer(source, replacement);
    }
    assert(unmaps == 1 && gSafeNearPagePool.pages.empty());
    auto stale = GenerateSafeNearTrampolineBuffer(source, replacement);
    const auto stale_slot = stale->strict_near_slot;
    ReleaseSafeNearTrampoline(stale);
    auto reused = GenerateSafeNearTrampolineBuffer(source, replacement);
    assert(reused->strict_near_slot == stale_slot);
    ReleaseSafeNearTrampoline(stale);
    assert(gSafeNearPagePool.pages[0].slots[0] == 1);
    Delete(reused);
    delete stale;
    auto first = GenerateSafeNearTrampolineBuffer(source, replacement);
    const auto published_slot = first->strict_near_slot;
    PublishSafeNearTrampoline(first);
    addr_t released_slot = 0;
    {
        TestRouting routing;
        routing.near_trampoline = GenerateSafeNearTrampolineBuffer(source, replacement);
        released_slot = routing.near_trampoline->strict_near_slot;
    }
    assert(unmaps == 3 && gSafeNearPagePool.pages.size() == 1);
    auto second = GenerateSafeNearTrampolineBuffer(source, replacement);
    assert(second->strict_near_slot == released_slot && second->strict_near_slot != published_slot);
    ReleaseSafeNearTrampoline(first);
    assert(first->strict_near_slot == 0);
    ReleaseSafeNearTrampoline(first); // stale token cannot release a new reservation
    assert(second->strict_near_slot == released_slot);
    Delete(second);
    delete first;
}
static void TestSlotReachabilityAndStableTokens() {
    Reset();
    const addr_t unaligned_source = source + 4;
    const addr_t edge = source + ARM64_B_XXX_RANGE;
    regions = {{source - ARM64_B_XXX_RANGE, edge},
               {edge + static_cast<uintptr_t>(test_page_size), edge + test_page_size * 2}};
    auto first = GenerateSafeNearTrampolineBuffer(unaligned_source, replacement);
    assert(first && first->strict_near_slot == edge);
    PublishSafeNearTrampoline(first);
    assert(!GenerateSafeNearTrampolineBuffer(unaligned_source, replacement));
    Delete(first);

    Reset();
    first = GenerateSafeNearTrampolineBuffer(source, replacement);
    const auto token = first->strict_near_slot;
    // Force vector growth without retaining references. Disjoint target clusters
    // create pages which can be erased before the old token is finally released.
    std::vector<Trampoline *> others;
    regions.clear();
    for (unsigned index = 1; index <= 32; ++index) {
        auto target = source + static_cast<addr_t>(index) * (1ull << 30);
        others.push_back(GenerateSafeNearTrampolineBuffer(target, target + (1ull << 29)));
        assert(others.back());
    }
    Delete(others.front());
    others.erase(others.begin());
    assert(first->strict_near_slot == token);
    Delete(first);
    for (auto trampoline : others)
        Delete(trampoline);
    assert(gSafeNearPagePool.pages.empty());
}
static void TestPatchFailures() {
    Reset();
    auto neighbor = GenerateSafeNearTrampolineBuffer(source, replacement);
    assert(PublishSafeNearTrampoline(neighbor));
    forward_extra_bytes = 4;
    last_patch = 0;
    assert(!GenerateSafeNearTrampolineBuffer(source, replacement));
    assert(last_patch == 0 && restores == 0 && unmaps == 0);
    assert(gSafeNearPagePool.pages[0].slots[0] == 2 && gSafeNearPagePool.pages[0].slots[1] == 0);
    assert(std::strcmp(dejavu_dobby::LastHookFailureStage(), "forward-patch") == 0);
    Delete(neighbor);

    for (int status : {-1, -2}) {
        Reset();
        patch_status = status;
        assert(!GenerateSafeNearTrampolineBuffer(source, replacement));
        assert(restores == 1 && unmaps == 1 && gSafeNearPagePool.pages.empty());
        assert(std::strcmp(dejavu_dobby::LastHookFailureStage(), "forward-patch") == 0);

        Reset();
        auto published = GenerateSafeNearTrampolineBuffer(source, replacement);
        PublishSafeNearTrampoline(published);
        patch_status = status;
        fail_restore = true;
        assert(!GenerateSafeNearTrampolineBuffer(source, replacement));
        assert(restores == 1 && unmaps == 0 && gSafeNearPagePool.pages[0].quarantined);
        patch_status = 0;
        fail_restore = false;
        assert(!GenerateSafeNearTrampolineBuffer(source, replacement));
        Delete(published);
        assert(unmaps == 0);
    }
    Reset();
    fail_unmap = fail_restore = true;
    patch_status = -2;
    assert(!GenerateSafeNearTrampolineBuffer(source, replacement));
    assert(unmaps == 1 && gSafeNearPagePool.pages.size() == 1 &&
           gSafeNearPagePool.pages[0].quarantined);
    patch_status = 0;
    assert(!GenerateSafeNearTrampolineBuffer(source, replacement));

    Reset();
    fail_unmap = true;
    auto trampoline = GenerateSafeNearTrampolineBuffer(source, replacement);
    Delete(trampoline);
    assert(unmaps == 1 && gSafeNearPagePool.pages.size() == 1 &&
           gSafeNearPagePool.pages[0].quarantined);
    assert(!GenerateSafeNearTrampolineBuffer(source, replacement));

    Reset();
    auto first = GenerateSafeNearTrampolineBuffer(source, replacement);
    auto second = GenerateSafeNearTrampolineBuffer(source, replacement);
    fail_restore = true;
    patch_status = -1;
    assert(!GenerateSafeNearTrampolineBuffer(source, replacement));
    assert(unmaps == 0); // neither reserved neighbor may be unmapped
    assert(!PublishSafeNearTrampoline(first));
    assert(!PublishSafeNearTrampoline(second));
    Delete(first);
    assert(unmaps == 0);
    Delete(second);
    assert(unmaps == 1);
}
int main() {
    Trampoline compatibility(FORWARD_TRAMPOLINE_ARM64, MemBlock(0x1000, 16));
    assert(compatibility.strict_near_slot == 0);
    PublishSafeNearTrampoline(&compatibility);
    ReleaseSafeNearTrampoline(&compatibility);
    auto direct = GenerateSafeNearTrampolineBuffer(source, source + 4);
    assert(direct && direct->strict_near_slot == 0);
    Delete(direct);
    for (long page : {4096, 16384}) {
        test_page_size = page;
        TestSharingAndCapacity();
        TestRollback();
        TestSlotReachabilityAndStableTokens();
        TestPatchFailures();
    }
    return 0;
}
