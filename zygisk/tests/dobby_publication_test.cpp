#include <cassert>
#include <cstdint>
#include <atomic>
#include <thread>
#include <dobby.h>
#include <InterceptRouting/InlineHookRouting.h>

size_t test_patch_size = 4;
bool test_prepare_fails = false;
static int activation_result = 0;
bool test_was_activated = false;
void *test_published_original = nullptr;
static bool restore_fails = false;
static std::atomic<void *> *prepared_slot = nullptr;
void test_activation_observer() {
    std::thread callback([] {
        assert(prepared_slot &&
               prepared_slot->load(std::memory_order_acquire) == reinterpret_cast<void *>(0x10000));
    });
    callback.join();
}
int DobbyCodePatch(void *, uint8_t *buffer, uint32_t) {
    if (buffer != reinterpret_cast<uint8_t *>(0x30000))
        return restore_fails ? -1 : 0;
    test_was_activated = true;
    if (prepared_slot)
        test_activation_observer();
    return activation_result;
}

static void prepared(void *original, size_t size, void *opaque) {
    assert(!test_was_activated);
    assert(size == 4);
    assert(original == reinterpret_cast<void *>(0x10000));
    auto *slot = static_cast<std::atomic<void *> *>(opaque);
    slot->store(original, std::memory_order_release);
    prepared_slot = slot;
    test_published_original = original;
}

int main() {
    std::atomic<void *> original{nullptr};
    assert(DobbyHookWithPrepare(reinterpret_cast<void *>(0x20000),
                                reinterpret_cast<void *>(0x30000), prepared, &original) == 0);
    assert(test_was_activated && DobbyIsHooked(reinterpret_cast<void *>(0x20000)) == 1);
    restore_fails = true;
    assert(DobbyDestroy(reinterpret_cast<void *>(0x20000)) != 0);
    assert(DobbyIsHooked(reinterpret_cast<void *>(0x20000)) == 1);
    restore_fails = false;
    assert(DobbyDestroy(reinterpret_cast<void *>(0x20000)) == 0);
    assert(DobbyIsHooked(reinterpret_cast<void *>(0x20000)) == 0);

    test_was_activated = false;
    test_prepare_fails = true;
    original.store(nullptr);
    assert(DobbyHookWithPrepare(reinterpret_cast<void *>(0x40000),
                                reinterpret_cast<void *>(0x30000), prepared, &original) != 0);
    assert(!test_was_activated && original.load() == nullptr);
    assert(DobbyIsHooked(reinterpret_cast<void *>(0x40000)) == 0);
    test_prepare_fails = false;

    test_patch_size = 16;
    assert(DobbyHookWithPrepare(reinterpret_cast<void *>(0x50000),
                                reinterpret_cast<void *>(0x30000), prepared, &original) != 0);
    assert(!test_was_activated && DobbyIsHooked(reinterpret_cast<void *>(0x50000)) == 0);
    test_patch_size = 4;
    activation_result = -2;
    assert(DobbyHookWithPrepare(reinterpret_cast<void *>(0x60000),
                                reinterpret_cast<void *>(0x30000), prepared, &original) != 0);
    assert(test_was_activated && original.load() != nullptr);
    assert(DobbyIsHooked(reinterpret_cast<void *>(0x60000)) == 1);
    restore_fails = true;
    assert(DobbyDestroy(reinterpret_cast<void *>(0x60000)) != 0);
    assert(DobbyIsHooked(reinterpret_cast<void *>(0x60000)) == 1);
    restore_fails = false;
    assert(DobbyDestroy(reinterpret_cast<void *>(0x60000)) == 0);
    activation_result = 0;
    test_was_activated = false;
    assert(DobbyHookWithPrepare(reinterpret_cast<void *>(0x60002),
                                reinterpret_cast<void *>(0x30000), prepared, &original) != 0);
    assert(!test_was_activated);

    // Preparation succeeded, but activation failed before touching target bytes.
    // Even a failing subsequent cleanup must never turn this into a live hook.
    activation_result = -1;
    restore_fails = true;
    original.store(nullptr);
    test_was_activated = false;
    assert(DobbyHookWithPrepare(reinterpret_cast<void *>(0x80000),
                                reinterpret_cast<void *>(0x30000), prepared, &original) != 0);
    assert(test_was_activated && original.load() != nullptr);
    assert(DobbyIsHooked(reinterpret_cast<void *>(0x80000)) == 0);
    assert(DobbyDestroy(reinterpret_cast<void *>(0x80000)) != 0);
    assert(DobbyIsHooked(reinterpret_cast<void *>(0x80000)) == 0);
    void *failed_original = reinterpret_cast<void *>(0x99900);
    assert(DobbyHook(reinterpret_cast<void *>(0x90000), reinterpret_cast<void *>(0x30000),
                     &failed_original) != 0);
    assert(failed_original == nullptr);
    assert(DobbyDestroy(reinterpret_cast<void *>(0x90000)) != 0);
    assert(DobbyIsHooked(reinterpret_cast<void *>(0x90000)) == 0);
    activation_result = 0;
    restore_fails = false;

    void *compatibility_original = nullptr;
    assert(DobbyHook(reinterpret_cast<void *>(0x70000), reinterpret_cast<void *>(0x30000),
                     &compatibility_original) == 0);
    assert(compatibility_original == reinterpret_cast<void *>(0x10000));
    assert(DobbyDestroy(reinterpret_cast<void *>(0x70000)) == 0);
}
