#include <atomic>
#include <cstdio>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

#define EXPORTED extern "C" __attribute__((visibility("default"), noinline))

namespace {
std::atomic<bool> running{false};
std::atomic<unsigned long long> calls{0}, errors{0};
pthread_t workers[2];
unsigned int worker_count = 0;
} // namespace

// Keep XOM away from the ELF header/dynamic tables that Android's linker reads
// during dladdr and from unrelated fixture helpers, including on 16 KiB pages.
EXPORTED __attribute__((aligned(16384))) long long native_fixture_target(long long value) {
    asm volatile("nop");
    return value + 7;
}

namespace {
void *worker(void *) {
    while (running.load(std::memory_order_acquire)) {
        if (native_fixture_target(9) != 16)
            errors.fetch_add(1, std::memory_order_relaxed);
        if ((calls.fetch_add(1, std::memory_order_relaxed) & 1023) == 0)
            sched_yield();
    }
    return nullptr;
}
} // namespace

EXPORTED long long native_fixture_start(long long) {
    if (worker_count != 0)
        return -1;
    calls.store(0);
    errors.store(0);
    running.store(true, std::memory_order_release);
    for (unsigned int i = 0; i < 2; ++i) {
        if (pthread_create(&workers[i], nullptr, worker, nullptr) != 0) {
            running.store(false, std::memory_order_release);
            for (unsigned int j = 0; j < worker_count; ++j)
                pthread_join(workers[j], nullptr);
            worker_count = 0;
            return -1;
        }
        ++worker_count;
    }
    return 0;
}

EXPORTED long long native_fixture_stop(long long) {
    running.store(false, std::memory_order_release);
    for (unsigned int i = 0; i < worker_count; ++i)
        pthread_join(workers[i], nullptr);
    worker_count = 0;
    return static_cast<long long>(errors.load(std::memory_order_acquire));
}

EXPORTED long long native_fixture_calls(long long) {
    return calls.load();
}
EXPORTED long long native_fixture_errors(long long) {
    return errors.load();
}

EXPORTED long long native_fixture_protect(long long protection) {
    const auto address = reinterpret_cast<uintptr_t>(native_fixture_target);
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || (protection != PROT_EXEC && protection != (PROT_READ | PROT_EXEC)))
        return -1;
    return mprotect(reinterpret_cast<void *>(address - address % page_size), page_size,
                    static_cast<int>(protection));
}

EXPORTED long long native_fixture_permissions(long long) {
    FILE *maps = std::fopen("/proc/self/maps", "re");
    if (maps == nullptr)
        return -1;
    const auto address = reinterpret_cast<uintptr_t>(native_fixture_target);
    char line[512];
    long long protection = -1;
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long start = 0, end = 0;
        char permissions[8] = {};
        if (std::sscanf(line, "%lx-%lx %7s", &start, &end, permissions) != 3 || address < start ||
            address >= end)
            continue;
        protection = (permissions[0] == 'r' ? PROT_READ : 0) |
                     (permissions[1] == 'w' ? PROT_WRITE : 0) |
                     (permissions[2] == 'x' ? PROT_EXEC : 0);
        break;
    }
    std::fclose(maps);
    return protection;
}
