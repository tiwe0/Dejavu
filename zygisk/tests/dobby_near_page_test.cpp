#include <cassert>
#include <cstdint>
#include <vector>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
using addr_t = uintptr_t;
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
static int mapping_mode = 0, maps = 0, unmaps = 0;
static long test_page_size = 4096;
static std::vector<uintptr_t> candidates;
static std::vector<std::string> diagnostics;
static void TestErrorLog(const char *format, ...) {
    char text[512];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    diagnostics.emplace_back(text);
}
#define ERROR_LOG(...) TestErrorLog(__VA_ARGS__)
static void *TestMmap(void *hint, size_t size, int protection, int flags, int fd, int offset) {
    assert(size == static_cast<size_t>(test_page_size));
    assert(protection == (PROT_READ | PROT_EXEC));
    assert((flags & MAP_ANONYMOUS) && (flags & MAP_PRIVATE) && !(flags & MAP_FIXED));
    assert(fd == -1 && offset == 0);
    ++maps;
    candidates.push_back(reinterpret_cast<uintptr_t>(hint));
    if (mapping_mode == 1 || (mapping_mode == 3 && maps == 1) || (mapping_mode == 6 && maps <= 4)) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    if (mapping_mode == 2 || (mapping_mode == 4 && maps == 1))
        return reinterpret_cast<void *>(0x1000);
    if (mapping_mode == 5)
        return reinterpret_cast<char *>(hint) + size;
    return hint;
}
static int TestMunmap(void *, size_t) {
    ++unmaps;
    return 0;
}
static long TestSysconf(int name) {
    assert(name == _SC_PAGESIZE);
    return test_page_size;
}
#define mmap TestMmap
#define munmap TestMunmap
#define sysconf TestSysconf
#include DOBBY_SAFE_NEAR_HELPERS
#undef mmap
#undef munmap
#undef sysconf
static void TestAllocator() {
    mapping_mode = maps = unmaps = 0;
    candidates.clear();
    diagnostics.clear();
    const uintptr_t source = 0x100000000;
    const uintptr_t lower = source - ARM64_B_XXX_RANGE;
    const uintptr_t upper = source + ARM64_B_XXX_RANGE;
    const uintptr_t size = static_cast<uintptr_t>(test_page_size);
    assert(SafeBranchReachable(source, lower));
    assert(!SafeBranchReachable(source, upper));
    assert(SafeBranchReachable(source, upper - 4));
    regions = {{lower + size, upper}};
    assert(AllocateSafeNearPage(source) == reinterpret_cast<void *>(lower));
    assert(maps == 1 && unmaps == 0);
    regions = {{lower, upper}};
    maps = unmaps = 0;
    assert(AllocateSafeNearPage(source) == nullptr && maps == 0);
    regions = {{lower + size, upper}};
    mapping_mode = 1;
    assert(AllocateSafeNearPage(source) == nullptr && maps == 1);
    assert(diagnostics[diagnostics.size() - 2].find("errno=" + std::to_string(ENOMEM)) != std::string::npos);
    maps = 0;
    mapping_mode = 2;
    assert(AllocateSafeNearPage(source) == nullptr && maps == 1 && unmaps == 1);

    // Only the forward trampoline entry must fit the branch displacement. Its
    // backing page may extend past the positive branch limit into a real gap.
    mapping_mode = 0;
    maps = unmaps = 0;
    const uintptr_t unaligned_source = source + 4;
    const uintptr_t final_page = (unaligned_source + ARM64_B_XXX_RANGE) / size * size;
    regions = {{lower, final_page}, {final_page + size, upper + size * 2}};
    assert(SafeBranchReachable(unaligned_source, final_page));
    assert(AllocateSafeNearPage(unaligned_source) == reinterpret_cast<void *>(final_page));
    assert(maps == 1 && unmaps == 0);

    regions = {{lower, final_page}};
    maps = unmaps = 0;
    assert(AllocateSafeNearPage(unaligned_source) == reinterpret_cast<void *>(final_page));
    assert(maps == 1 && unmaps == 0);

    // A raced or ignored first hint must not hide the second free page in the
    // same small gap. Out-of-range mappings are returned before retrying.
    regions = {{lower + size * 2, upper}};
    for (int mode : {3, 4}) {
        mapping_mode = mode;
        maps = unmaps = 0;
        candidates.clear();
        assert(AllocateSafeNearPage(source) == reinterpret_cast<void *>(lower + size));
        assert(maps == 2 && unmaps == (mode == 4 ? 1 : 0));
        assert(candidates == std::vector<uintptr_t>({lower, lower + size}));
    }

    // mmap's hint is not a requirement: any newly allocated reachable page is
    // suitable, even when the kernel chooses a different address.
    mapping_mode = 5;
    maps = unmaps = 0;
    assert(AllocateSafeNearPage(source) == reinterpret_cast<void *>(lower + size));
    assert(maps == 1 && unmaps == 0);

    // Exhaust the local retry allowance, then continue at the next real gap.
    mapping_mode = 6;
    maps = unmaps = 0;
    candidates.clear();
    regions = {{lower + size * 8, lower + size * 9}, {lower + size * 11, upper}};
    assert(AllocateSafeNearPage(source) == reinterpret_cast<void *>(lower + size * 9));
    assert(maps == 5 && candidates[3] == lower + size * 3 && candidates[4] == lower + size * 9);

    // Bound failed allocation work rather than issuing tens of thousands of
    // mmap calls for a large hole. Still try later gaps after local retries.
    mapping_mode = 1;
    maps = unmaps = 0;
    regions = {{upper, upper + size}};
    assert(AllocateSafeNearPage(source) == nullptr && maps == 4);
    maps = 0;
    regions.clear();
    for (uintptr_t position = lower + size * 2; position < upper; position += size * 3)
        regions.push_back({position, position + size});
    assert(AllocateSafeNearPage(source) == nullptr && maps == 64);
}
int main() {
    for (long size : {4096, 16384}) {
        test_page_size = size;
        TestAllocator();
    }
}
