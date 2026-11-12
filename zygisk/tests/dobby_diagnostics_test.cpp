#include <atomic>
#include <cassert>
#include <cstring>
#include <thread>
#include <dobby/dejavu_hook_diagnostics.h>

extern const char *TestReadDobbyStage();
extern void TestSetDobbyStage(const char *stage);

int main() {
    assert(dejavu_dobby::LastHookFailureStage() == nullptr);
    TestSetDobbyStage("near-allocation");
    assert(std::strcmp(dejavu_dobby::LastHookFailureStage(), "near-allocation") == 0);
    std::atomic<bool> ready{false}, finish{false};
    std::thread worker([&] {
        assert(TestReadDobbyStage() == nullptr);
        dejavu_dobby::SetHookFailureStage("forward-patch");
        ready.store(true, std::memory_order_release);
        while (!finish.load(std::memory_order_acquire))
            std::this_thread::yield();
        assert(std::strcmp(TestReadDobbyStage(), "forward-patch") == 0);
        dejavu_dobby::ResetHookFailureStage();
        assert(TestReadDobbyStage() == nullptr);
    });
    while (!ready.load(std::memory_order_acquire))
        std::this_thread::yield();
    assert(std::strcmp(TestReadDobbyStage(), "near-allocation") == 0);
    dejavu_dobby::ResetHookFailureStage();
    assert(TestReadDobbyStage() == nullptr);
    finish.store(true, std::memory_order_release);
    worker.join();
}
