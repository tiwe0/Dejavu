#pragma once
#include <Interceptor.h>
#include <cstring>
#include <dobby/dejavu_hook_diagnostics.h>
extern size_t test_patch_size;
extern bool test_prepare_fails;
extern const char *test_routing_failure_stage;
extern bool test_activate_fails;
extern bool test_was_activated;
extern bool test_code_published;
extern bool test_publish_fails;
extern void *test_published_original;
extern void test_activation_observer();
struct InterceptRouting {
    Interceptor::Entry *entry;
    int error = 0;
    bool require_atomic_patch = false;
    explicit InterceptRouting(Interceptor::Entry *entry) : entry(entry) {
    }
    virtual ~InterceptRouting() = default;
    bool PublishPreparedCode() {
#ifdef DOBBY_TEST_PUBLISH_OBSERVER
        if (!DOBBY_TEST_PUBLISH_OBSERVER())
            return false;
#endif
        if (test_publish_fails)
            return false;
        test_code_published = true;
        return true;
    }
    virtual addr_t TrampolineTarget() {
        return 0;
    }
    bool GenerateTrampoline() {
#ifdef DOBBY_TEST_BUILD_OBSERVER
        if (!DOBBY_TEST_BUILD_OBSERVER())
            return false;
#endif
        if (test_prepare_fails && test_routing_failure_stage)
            dejavu_dobby::SetHookFailureStage(test_routing_failure_stage);
        return !test_prepare_fails;
    }
    void GenerateRelocatedCode() {
        if (test_routing_failure_stage &&
            std::strcmp(test_routing_failure_stage, "original-relocation") == 0) {
            dejavu_dobby::SetHookFailureStage(test_routing_failure_stage);
            error = 1;
            return;
        }
        entry->patched = MemBlock(entry->addr, test_patch_size);
        entry->relocated = MemBlock(0x10000, 16);
    }
    void BackupOriginCode() {
        entry->origin_code_ = static_cast<uint8_t *>(::operator new(test_patch_size));
    }
    size_t trampoline_size() {
        return test_patch_size;
    }
    addr_t trampoline_addr() {
        return 0x30000;
    }
#include <dobby-active.h>
};
