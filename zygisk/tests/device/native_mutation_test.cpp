// Standalone regression against actual linked lifecycle functions.
#include <dobby.h>
#include <dobby/dejavu_hook_diagnostics.h>
#include <dobby/dejavu_hook_mutation.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <sys/mman.h>
#include <unistd.h>

namespace {
using Function = int (*)();
enum class Phase { Idle, Mapping, Protection, Unmapping, Preparation };
Phase armed = Phase::Idle;
bool fail_write = false;
unsigned probes = 0;
void *outer_target = nullptr, *nested_target = nullptr, *keep_target = nullptr;
std::atomic<Function> outer_original{nullptr}, keep_original{nullptr};
void check(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
int outer_replacement() {
    return outer_original.load()() + 100;
}
int keep_replacement() {
    return keep_original.load()() + 200;
}
int nested_replacement() {
    return 7;
}
void nested_prepare(void *, size_t, void *) {
    check(false, "nested preparation must not run");
}
void instrument(void *, DobbyRegisterContext *) {
    check(false, "nested instrumentation must not run");
}
void probe() {
    armed = Phase::Idle;
    ++probes;
    check(DobbyIsHooked(keep_target) == 1, "read-only query remains available");
    const char *previous = dejavu_dobby::LastHookFailureStage();
    dejavu_dobby::SetHookFailureStage("outer-test-diagnostic");
    const char *sentinel = dejavu_dobby::LastHookFailureStage();
    check(DobbyHookWithPrepare(nested_target, reinterpret_cast<void *>(nested_replacement),
                               nested_prepare, nullptr) == dejavu_dobby::kHookMutationReentrant,
          "nested prepared installation must be rejected");
    void *original = reinterpret_cast<void *>(uintptr_t{0x1234});
    check(DobbyHook(nested_target, reinterpret_cast<void *>(nested_replacement), &original) ==
                  dejavu_dobby::kHookMutationReentrant &&
              original == reinterpret_cast<void *>(uintptr_t{0x1234}),
          "nested compatibility installation must preserve output");
    check(DobbyInstrument(nested_target, instrument) == dejavu_dobby::kHookMutationReentrant,
          "nested instrumentation must be rejected");
    check(DobbyDestroy(keep_target) == dejavu_dobby::kHookMutationReentrant &&
              DobbyDestroy(outer_target) == dejavu_dobby::kHookMutationReentrant,
          "nested removal preserves existing and pending hooks");
    check(dejavu_dobby::LastHookFailureStage() == sentinel, "preserve outer diagnosis");
    check(DobbyIsHooked(keep_target) == 1, "preserve existing hook");
    dejavu_dobby::SetHookFailureStage(previous);
}
void prepare(void *function, size_t size, void *context) {
    check(size == 4, "entry remains one instruction");
    static_cast<std::atomic<Function> *>(context)->store(reinterpret_cast<Function>(function));
    if (armed == Phase::Preparation)
        probe();
}
void *target() {
    constexpr size_t range = size_t{1} << 27;
    const auto page = static_cast<size_t>(getpagesize());
    void *reservation = mmap(nullptr, 2 * range + 2 * page, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    check(reservation != MAP_FAILED, "reserve isolated virtual range");
    auto base = reinterpret_cast<uintptr_t>(reservation);
    auto *code = reinterpret_cast<uint32_t *>(base + range);
    check(mprotect(code, page, PROT_READ | PROT_WRITE) == 0, "create target");
    code[0] = 0x52800540;
    code[1] = 0xd65f03c0;
    code[2] = 0x52800560;
    code[3] = 0xd65f03c0;
    __builtin___clear_cache(reinterpret_cast<char *>(code), reinterpret_cast<char *>(code + 4));
    check(mprotect(code, page, PROT_READ | PROT_EXEC) == 0, "target executable");
    check(munmap(reinterpret_cast<void *>(base + range + 4 * page), page) == 0,
          "release unique near page");
    // Keep targets and published slots mapped until this process exits.
    return code;
}
} // namespace

extern "C" void *__real_mmap(void *, size_t, int, int, int, off_t);
extern "C" int __real_mprotect(void *, size_t, int);
extern "C" int __real_munmap(void *, size_t);
extern "C" void *__wrap_mmap(void *address, size_t size, int protection, int flags, int fd,
                             off_t offset) {
    if (armed == Phase::Mapping)
        probe();
    return __real_mmap(address, size, protection, flags, fd, offset);
}
extern "C" int __wrap_mprotect(void *address, size_t size, int protection) {
    if (armed == Phase::Protection)
        probe();
    if (fail_write && (protection & PROT_WRITE)) {
        fail_write = false;
        errno = EACCES;
        return -1;
    }
    return __real_mprotect(address, size, protection);
}
extern "C" int __wrap_munmap(void *address, size_t size) {
    if (armed == Phase::Unmapping)
        probe();
    return __real_munmap(address, size);
}

int main() {
    keep_target = target();
    nested_target = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(keep_target) + 8);
    check(DobbyHookWithPrepare(keep_target, reinterpret_cast<void *>(keep_replacement), prepare,
                               &keep_original) == 0,
          "keep hook install");
    for (Phase phase : {Phase::Mapping, Phase::Protection, Phase::Unmapping, Phase::Preparation}) {
        outer_target = target();
        outer_original.store(nullptr);
        const unsigned previous_probes = probes;
        armed = phase;
        fail_write = phase == Phase::Unmapping;
        const int status = DobbyHookWithPrepare(
            outer_target, reinterpret_cast<void *>(outer_replacement), prepare, &outer_original);
        check(probes == previous_probes + 1 && armed == Phase::Idle, "probe executes once");
        if (phase == Phase::Unmapping) {
            const char *stage = dejavu_dobby::LastHookFailureStage();
            check(status != 0 && !outer_original.load() && DobbyIsHooked(outer_target) == 0 &&
                      stage && std::strcmp(stage, "forward-patch") == 0,
                  "write failure keeps diagnosis and rolls back before publication");
        } else {
            check(status == 0 && outer_original.load()() == 42 &&
                      reinterpret_cast<Function>(outer_target)() == 142,
                  "outer hook and original callable after nested requests");
            check(DobbyDestroy(outer_target) == 0, "outer hook uninstall");
        }
        check(reinterpret_cast<Function>(outer_target)() == 42, "outer target restored");
        check(reinterpret_cast<Function>(keep_target)() == 242, "existing hook preserved");
    }
    check(DobbyDestroy(keep_target) == 0 && reinterpret_cast<Function>(keep_target)() == 42,
          "normal uninstall works after rejected nested requests");
    std::puts(
        "PASS: real Dobby APIs reject nested OS/callback mutations; hooks and rollback survive");
}
