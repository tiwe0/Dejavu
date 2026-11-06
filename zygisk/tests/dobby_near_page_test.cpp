#include <cassert>
#include <cstdint>
#include <vector>
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
static void *TestMmap(void *hint, size_t size, int protection, int flags, int fd, int offset) {
    assert(size == static_cast<size_t>(sysconf(_SC_PAGESIZE)));
    assert(protection == (PROT_READ | PROT_EXEC));
    assert((flags & MAP_ANONYMOUS) && (flags & MAP_PRIVATE) && !(flags & MAP_FIXED));
    assert(fd == -1 && offset == 0);
    ++maps;
    return mapping_mode == 1   ? MAP_FAILED
           : mapping_mode == 2 ? reinterpret_cast<void *>(0x1000)
                               : hint;
}
static int TestMunmap(void *, size_t) {
    ++unmaps;
    return 0;
}
#define mmap TestMmap
#define munmap TestMunmap
#include DOBBY_SAFE_NEAR_HELPERS
#undef mmap
#undef munmap
int main() {
    const uintptr_t source = 0x100000000;
    const uintptr_t lower = source - ARM64_B_XXX_RANGE;
    const uintptr_t upper = source + ARM64_B_XXX_RANGE;
    const uintptr_t size = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
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
    maps = 0;
    mapping_mode = 2;
    assert(AllocateSafeNearPage(source) == nullptr && maps == 1 && unmaps == 1);
}
