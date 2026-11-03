#include <jni.h>
#include <sys/mman.h>
#include <sys/uio.h>

#include <atomic>
#include <cassert>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "root_helper_client.h"
#include "stream_service.h"
#include "dejavu.h"
#include "dejavu_plugin.h"

// Access fixture state without replacing any of PluginManager's hook logic.
#define private public
#include "plugin_manager.h"
#undef private

#if defined(__APPLE__)
extern "C" ssize_t process_vm_readv(pid_t, const iovec *, unsigned long,
                                      const iovec *, unsigned long, unsigned long);
extern "C" ssize_t process_vm_writev(pid_t, const iovec *, unsigned long,
                                       const iovec *, unsigned long, unsigned long);
#endif

#include "plugin_manager.cpp"

namespace {
struct Scenario {
    std::set<void *> installed;
    bool install_error_after_activation = false;
    bool destroy_failure = false;
    bool apply_failure = false;
    unsigned int restore_failures = 0;
    unsigned int captures = 0;
    unsigned int destroys = 0;
    unsigned int restores = 0;
    unsigned int modules_destroyed = 0;
    unsigned int prepares = 0;
    unsigned int shutdowns = 0;
} scenario;

void prepare_plugin() { ++scenario.prepares; }
void shutdown_plugin() { ++scenario.shutdowns; }
void destroy_module(dejavu_module *) { ++scenario.modules_destroyed; }

struct Fixture {
    DejavuRuntime runtime{};
    PluginManager manager;
    Fixture() {
        scenario = {};
        runtime.module_destroy = destroy_module;
        manager.runtime_ = &runtime;
        add_plugin("first");
        g_current_plugin_index = 0;
    }
    void add_plugin(const char *name) {
        auto plugin = std::make_unique<PluginManager::Plugin>();
        plugin->name = name;
        plugin->loaded = true;
        plugin->initialized = true;
        plugin->flags = DEJAVU_PLUGIN_FLAG_UNLOAD_SAFE;
        plugin->jit = true;
        plugin->jit_module = reinterpret_cast<dejavu_module *>(0x1000 + manager.plugins_.size());
        plugin->jit_prepare = prepare_plugin;
        plugin->jit_shutdown = shutdown_plugin;
        manager.plugins_.push_back(std::move(plugin));
    }
    ~Fixture() {
        scenario.destroy_failure = false;
        scenario.apply_failure = false;
        scenario.restore_failures = 0;
        manager.shutdown();
        g_current_plugin_index = kPluginIndexNone;
    }
    void install(void *target = reinterpret_cast<void *>(0x1120)) {
        char error[512]{};
        void *original = nullptr;
        assert(manager.install_owned_hook(target, reinterpret_cast<void *>(0x8000),
                                           &original, error, sizeof(error)) == DEJAVU_OK);
        assert(original == reinterpret_cast<void *>(0x7000));
    }
};

void test_destroy_failure_retains_module_until_retry() {
    Fixture fixture;
    fixture.install();
    scenario.destroy_failure = true;
    std::string status, error;
    assert(!fixture.manager.unload_plugin("first", &status, &error));
    assert(error.find("DobbyDestroy failed") != std::string::npos);
    assert(fixture.manager.plugins_.size() == 1 && fixture.manager.plugins_[0]->loaded);
    assert(fixture.manager.hooks_.size() == 1 && scenario.modules_destroyed == 0);
    assert(fixture.manager.status().find("loaded=yes jit=yes caps=") != std::string::npos);
    assert(scenario.prepares == 1 && scenario.shutdowns == 0);
    scenario.destroy_failure = false;
    assert(fixture.manager.unload_plugin("first", &status, &error));
    assert(fixture.manager.plugins_.empty() && fixture.manager.hooks_.empty());
    assert(scenario.modules_destroyed == 1 && scenario.shutdowns == 1 && scenario.prepares == 1);
}

void test_restore_failure_retries_without_destroying_twice() {
    Fixture fixture;
    fixture.install();
    scenario.restore_failures = 1;
    char error[512]{};
    assert(!fixture.manager.uninstall_owned_hook(reinterpret_cast<void *>(0x1120), error, sizeof(error)));
    assert(fixture.manager.hooks_.size() == 1);
    assert(!fixture.manager.hooks_[0].installed && fixture.manager.hooks_[0].permissions_dirty);
    assert(scenario.destroys == 1);
    assert(fixture.manager.uninstall_owned_hook(reinterpret_cast<void *>(0x1120), error, sizeof(error)));
    assert(fixture.manager.hooks_.empty() && scenario.destroys == 1);
}

void test_failed_install_and_shutdown_keep_exposed_code() {
    Fixture fixture;
    scenario.install_error_after_activation = true;
    scenario.destroy_failure = true;
    char error[512]{};
    void *original = nullptr;
    assert(fixture.manager.install_owned_hook(reinterpret_cast<void *>(0x1120),
        reinterpret_cast<void *>(0x8000), &original, error, sizeof(error)) != DEJAVU_OK);
    assert(fixture.manager.hooks_.size() == 1 && fixture.manager.hooks_[0].installed);
    fixture.manager.shutdown();
    assert(fixture.manager.plugins_.size() == 1 && fixture.manager.plugins_[0]->loaded);
    assert(scenario.modules_destroyed == 0 && scenario.shutdowns == 0);
    scenario.destroy_failure = false;
    fixture.manager.shutdown();
    assert(fixture.manager.plugins_.empty() && fixture.manager.hooks_.empty());
    assert(scenario.modules_destroyed == 1);
}

void test_foreign_hook_is_not_removed() {
    Fixture fixture;
    void *target = reinterpret_cast<void *>(0x1120);
    scenario.installed.insert(target);
    char error[512]{};
    void *original = nullptr;
    assert(fixture.manager.install_owned_hook(target, reinterpret_cast<void *>(0x8000),
        &original, error, sizeof(error)) == DEJAVU_ERROR_INVALID_ARGUMENT);
    assert(fixture.manager.hooks_.empty() && scenario.captures == 0 && scenario.destroys == 0);
    assert(scenario.installed.count(target) == 1);
}

void test_failed_initializer_cleanup_and_index_shift() {
    Fixture fixture;
    fixture.add_plugin("second");
    fixture.install();
    g_current_plugin_index = 1;
    fixture.install(reinterpret_cast<void *>(0x2120));
    g_current_plugin_index = 0;
    std::string status, error;
    assert(fixture.manager.unload_plugin("first", &status, &error));
    assert(fixture.manager.hooks_.size() == 1 && fixture.manager.hooks_[0].plugin_index == 0);
    assert(fixture.manager.unload_plugin("second", &status, &error));
    assert(fixture.manager.plugins_.empty() && scenario.modules_destroyed == 2);
}

void test_unpatched_permission_cleanup_remains_owned() {
    Fixture fixture;
    scenario.apply_failure = true;
    scenario.restore_failures = 1;
    char error[512]{};
    void *original = nullptr;
    assert(fixture.manager.install_owned_hook(reinterpret_cast<void *>(0x1120),
        reinterpret_cast<void *>(0x8000), &original, error, sizeof(error)) != DEJAVU_OK);
    assert(fixture.manager.hooks_.size() == 1 && !fixture.manager.hooks_[0].installed);
    fixture.manager.plugins_[0]->initialized = false;
    fixture.manager.plugins_[0]->cleanup_pending = true;
    scenario.apply_failure = false;
    std::string status, detail;
    assert(fixture.manager.unload_plugin("first", &status, &detail));
    assert(scenario.prepares == 0 && scenario.shutdowns == 0 && scenario.destroys == 0);
    assert(scenario.modules_destroyed == 1);
}

void test_failed_initializer_without_unload_safe_retries_host_cleanup() {
    for (bool fail_destroy : {true, false}) {
        Fixture fixture;
        fixture.install();
        auto &plugin = *fixture.manager.plugins_[0];
        plugin.flags = 0;
        plugin.initialized = false;
        plugin.cleanup_pending = true;
        scenario.destroy_failure = fail_destroy;
        scenario.restore_failures = fail_destroy ? 0 : 1;
        std::string status, error;
        assert(!fixture.manager.unload_plugin("first", &status, &error));
        assert(fixture.manager.plugins_.size() == 1 && fixture.manager.hooks_.size() == 1);
        assert(scenario.modules_destroyed == 0 && scenario.prepares == 0 && scenario.shutdowns == 0);
        assert(error.find(fail_destroy ? "DobbyDestroy failed" : "permission restore failure") !=
               std::string::npos);
        scenario.destroy_failure = false;
        assert(fixture.manager.unload_plugin("first", &status, &error));
        assert(fixture.manager.plugins_.empty() && fixture.manager.hooks_.empty());
        assert(scenario.modules_destroyed == 1 && scenario.prepares == 0 && scenario.shutdowns == 0);
        assert(scenario.destroys == (fail_destroy ? 2u : 1u));
    }
}

void test_initialized_unsafe_plugin_keeps_unload_restriction() {
    Fixture fixture;
    fixture.install();
    fixture.manager.plugins_[0]->flags = 0;
    for (bool pending : {false, true}) {
        fixture.manager.plugins_[0]->cleanup_pending = pending;
        std::string status, error;
        assert(!fixture.manager.unload_plugin("first", &status, &error));
        assert(error.find("not marked unload-safe") != std::string::npos);
        assert(fixture.manager.plugins_.size() == 1 && fixture.manager.hooks_.size() == 1);
        assert(scenario.destroys == 0 && scenario.modules_destroyed == 0);
        assert(scenario.prepares == 0 && scenario.shutdowns == 0);
    }
}
} // namespace

extern "C" int __android_log_print(int, const char *, const char *, ...) { return 0; }
extern "C" int __android_log_write(int, const char *, const char *) { return 0; }
extern "C" int DobbyIsHooked(void *target) { return scenario.installed.count(target) ? 1 : 0; }
extern "C" int DobbyHook(void *target, void *, void **original) {
    *original = reinterpret_cast<void *>(0x7000);
    scenario.installed.insert(target);
    return scenario.install_error_after_activation ? -1 : 0;
}
extern "C" int DobbyDestroy(void *target) {
    ++scenario.destroys;
    if (scenario.destroy_failure) return -1;
    return scenario.installed.erase(target) ? 0 : -1;
}

bool root_helper_capture(void *address, size_t size, RootHelperProtectionSnapshot *snapshot,
                         std::string *) {
    assert(size == 16);
    ++scenario.captures;
    const uintptr_t start = reinterpret_cast<uintptr_t>(address) & ~uintptr_t{0xfff};
    snapshot->ranges = {{start, 0x1000, PROT_EXEC}};
    return true;
}
bool root_helper_apply_read_write(const RootHelperProtectionSnapshot &, std::string *error) {
    if (!scenario.apply_failure) return true;
    *error = "injected permission apply failure";
    return false;
}
bool root_helper_restore(const RootHelperProtectionSnapshot &snapshot, std::string *error) {
    ++scenario.restores;
    assert(snapshot.ranges.size() == 1 && snapshot.ranges[0].length == 0x1000);
    assert(snapshot.ranges[0].protection == PROT_EXEC);
    if (scenario.restore_failures == 0) return true;
    --scenario.restore_failures;
    *error = "injected permission restore failure";
    return false;
}

StreamService::StreamService() = default;
StreamService::~StreamService() = default;
size_t StreamService::close_owner(uint64_t) { return 0; }

int main() {
    test_destroy_failure_retains_module_until_retry();
    test_restore_failure_retries_without_destroying_twice();
    test_failed_install_and_shutdown_keep_exposed_code();
    test_foreign_hook_is_not_removed();
    test_failed_initializer_cleanup_and_index_shift();
    test_unpatched_permission_cleanup_remains_owned();
    test_failed_initializer_without_unload_safe_retries_host_cleanup();
    test_initialized_unsafe_plugin_keeps_unload_restriction();
    std::puts("OK: plugin hook rollback, teardown retry and module retention");
}
