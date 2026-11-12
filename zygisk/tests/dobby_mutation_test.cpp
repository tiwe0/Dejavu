// Reuse the real near-pool allocator/routing and deterministic OS seams.
static unsigned log_count = 0;
static void ObserveLog() { ++log_count; }
#define DOBBY_TEST_LOG_OBSERVER ObserveLog
#define main PoolSafetyMain
#include "dobby_near_pool_test.cpp"
#undef main
#include <atomic>
#include <thread>
#include <new>
#include <cstdlib>

static size_t allocation_count = 0;
void *operator new(size_t size) {
    ++allocation_count;
    if (auto *value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void operator delete(void *value) noexcept { std::free(value); }
void operator delete(void *value, size_t) noexcept { std::free(value); }

static Trampoline *active_trampoline = nullptr;
static bool BuildObserved() {
    active_trampoline = GenerateSafeNearTrampolineBuffer(source, replacement);
    return active_trampoline != nullptr;
}
static bool PublishObserved() { return PublishSafeNearTrampoline(active_trampoline); }
#define DOBBY_TEST_BUILD_OBSERVER BuildObserved
#define DOBBY_TEST_PUBLISH_OBSERVER PublishObserved
#include <InterceptRouting/InlineHookRouting.h>
// Only the instrument routing machine is stubbed; its lifecycle entry point is
// extracted verbatim, including the same guard used in production.
struct InstrumentRouting : InlineHookRouting {
    InstrumentRouting(Interceptor::Entry *entry, dobby_instrument_callback_t)
        : InlineHookRouting(entry, replacement) {}
};
#include <dobby-instrument.h>
#include <dobby-lifecycle.h>

size_t test_patch_size = 4;
bool test_prepare_fails = false;
const char *test_routing_failure_stage = nullptr;
bool test_was_activated = false;
bool test_code_published = false;
bool test_publish_fails = false;
void *test_published_original = nullptr;
void test_activation_observer() {}
extern bool TestReadDobbyMutation();
static unsigned rejected_batches = 0, prepare_calls = 0;
static void *outer_target = reinterpret_cast<void *>(source);
static void NeverPrepare(void *, size_t, void *) { assert(false); }

static uint64_t PoolOwnership() {
    uint64_t state = 0;
    for (const auto &page : gSafeNearPagePool.pages) {
        state = state * 131 + page.address + page.size + page.quarantined;
        for (auto slot : page.slots) state = state * 131 + slot;
    }
    return state;
}
static void RejectNested() {
    const auto entries = gInterceptor.count();
    const auto pages = gSafeNearPagePool.pages.size();
    const auto ownership = PoolOwnership();
    const auto *page_data = gSafeNearPagePool.pages.data();
    const auto stage = dejavu_dobby::LastHookFailureStage();
    assert(TestReadDobbyMutation());
    const auto logs = log_count;
    const auto allocations = allocation_count;
    const auto map_count = maps, unmap_count = unmaps, restore_count = restores;
    for (void *target : {outer_target, reinterpret_cast<void *>(source + 0x10000), static_cast<void *>(nullptr)}) {
        void *output = reinterpret_cast<void *>(0xcafe);
        assert(DobbyHook(target, reinterpret_cast<void *>(replacement), &output) ==
               dejavu_dobby::kHookMutationReentrant);
        assert(output == reinterpret_cast<void *>(0xcafe));
        assert(DobbyHookWithPrepare(target, reinterpret_cast<void *>(replacement), NeverPrepare, nullptr) ==
               dejavu_dobby::kHookMutationReentrant);
        assert(DobbyInstrument(target, nullptr) == dejavu_dobby::kHookMutationReentrant);
        assert(DobbyDestroy(target) == dejavu_dobby::kHookMutationReentrant);
        assert(DobbyIsHooked(target) == (target == outer_target &&
               gInterceptor.find(source) && gInterceptor.find(source)->bytes_published ? 1 : 0));
    }
    assert(allocations == allocation_count && logs == log_count);
    assert(stage == dejavu_dobby::LastHookFailureStage());
    assert(entries == gInterceptor.count());
    assert(pages == gSafeNearPagePool.pages.size() && page_data == gSafeNearPagePool.pages.data());
    assert(ownership == PoolOwnership());
    assert(map_count == maps && unmap_count == unmaps && restore_count == restores);
    ++rejected_batches;
}
static void Prepare(void *original, size_t size, void *opaque) {
    assert(original == reinterpret_cast<void *>(0x10000) && size == 4);
    assert(gSafeNearPagePool.pages[0].slots[0] == 2);
    dejavu_dobby::SetHookFailureStage("outer-callback-diagnostic");
    RejectNested();
    *static_cast<void **>(opaque) = original;
    ++prepare_calls;
}
int main() {
    PoolSafetyMain();
    for (long page : {4096, 16384}) {
        test_page_size = page;
        Reset();
        test_os_observer = RejectNested;
        void *original = nullptr;
        assert(DobbyHookWithPrepare(outer_target, reinterpret_cast<void *>(replacement), Prepare, &original) == 0);
        assert(original == reinterpret_cast<void *>(0x10000));
        assert(std::strcmp(dejavu_dobby::LastHookFailureStage(), "outer-callback-diagnostic") == 0);
        assert(DobbyIsHooked(outer_target) == 1);
        assert(gSafeNearPagePool.pages.size() == 1 && gSafeNearPagePool.pages[0].slots[0] == 2);
        patch_status = -1;
        assert(DobbyDestroy(outer_target) == -1);
        assert(DobbyIsHooked(outer_target) == 1 && gSafeNearPagePool.pages[0].slots[0] == 2);
        patch_status = 0;
        assert(DobbyDestroy(outer_target) == 0);
        assert(DobbyIsHooked(outer_target) == 0 && unmaps == 0);
        test_os_observer = nullptr;
        Delete(active_trampoline); // published page remains owned for process lifetime
        assert(unmaps == 0 && gSafeNearPagePool.pages[0].slots[0] == 2);

        // Forward-patch rollback invokes mprotect and munmap while the outer
        // mutation is active; nested calls must preserve its failure stage.
        Reset(); patch_status = -1; test_os_observer = RejectNested;
        assert(DobbyHookWithPrepare(outer_target, reinterpret_cast<void *>(replacement), Prepare, &original) == -1);
        assert(std::strcmp(dejavu_dobby::LastHookFailureStage(), "forward-patch") == 0);
        assert(gSafeNearPagePool.pages.empty() && unmaps == 1 && restores == 1);
        assert(DobbyIsHooked(outer_target) == 0);
        test_os_observer = nullptr;

        // Ordinary installation and instrumentation enter the same scope and
        // release it after completion, including a failed predecessor.
        Reset(); test_os_observer = RejectNested;
        assert(DobbyHook(outer_target, reinterpret_cast<void *>(replacement), &original) == 0);
        assert(DobbyDestroy(outer_target) == 0);
        test_os_observer = nullptr; Delete(active_trampoline);
        Reset(); test_os_observer = RejectNested;
        assert(DobbyInstrument(outer_target, nullptr) == 0);
        assert(DobbyDestroy(outer_target) == 0);
        test_os_observer = nullptr; Delete(active_trampoline);
    }
    assert(prepare_calls == 2 && rejected_batches >= 20);
    assert(!dejavu_dobby::hook_mutation_in_progress);
}
