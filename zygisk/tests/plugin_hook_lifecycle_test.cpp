#include <jni.h>
#include <sys/mman.h>
#include <sys/uio.h>

#include <atomic>
#include <condition_variable>
#include <thread>
#include <dlfcn.h>
#include <limits.h>
#include <stdexcept>
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
extern "C" ssize_t process_vm_readv(pid_t, const iovec *, unsigned long, const iovec *,
                                    unsigned long, unsigned long);
extern "C" ssize_t process_vm_writev(pid_t, const iovec *, unsigned long, const iovec *,
                                     unsigned long, unsigned long);
#endif

void *plugin_test_dlsym(void *, const char *);
int plugin_test_dlclose(void *);
#define dlsym plugin_test_dlsym
#define dlclose plugin_test_dlclose
#include "plugin_manager.cpp"
#undef dlsym
#undef dlclose
#include "sha256.cpp"

namespace {
std::mutex stream_test_mutex;
std::map<int, uint64_t> stream_test_owners;
std::set<uint64_t> stream_test_retired;
int stream_test_next = 1;
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
    unsigned int result_releases = 0;
    unsigned int function_releases = 0;
} scenario;

void prepare_plugin() {
    ++scenario.prepares;
}
void shutdown_plugin() {
    ++scenario.shutdowns;
}
void destroy_module(dejavu_module *) {
    ++scenario.modules_destroyed;
}
void release_result(dejavu_value *value) {
    ++scenario.result_releases;
    if (value->type == DEJAVU_VALUE_STRING && value->owned)
        free(const_cast<char *>(value->string_value));
    *value = {};
}
void release_function(dejavu_control_session *, unsigned long long value) {
    assert(value == 77);
    ++scenario.function_releases;
}

struct Fixture {
    DejavuRuntime runtime{};
    PluginManager manager;
    Fixture() {
        scenario = {};
        stream_test_owners.clear();
        stream_test_retired.clear();
        g_worker_stream_context.reset();
        g_jit_manager = &manager;
        runtime.module_destroy = destroy_module;
        runtime.value_release = release_result;
        runtime.control_session_release_function = release_function;
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
        assert(manager.install_owned_hook(target, reinterpret_cast<void *>(0x8000), &original,
                                          error, sizeof(error)) == DEJAVU_OK);
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
    assert(!fixture.manager.uninstall_owned_hook(reinterpret_cast<void *>(0x1120), error,
                                                 sizeof(error)));
    assert(fixture.manager.hooks_.size() == 1);
    assert(!fixture.manager.hooks_[0].installed && fixture.manager.hooks_[0].permissions_dirty);
    assert(scenario.destroys == 1);
    assert(fixture.manager.uninstall_owned_hook(reinterpret_cast<void *>(0x1120), error,
                                                sizeof(error)));
    assert(fixture.manager.hooks_.empty() && scenario.destroys == 1);
}

void test_failed_install_and_shutdown_keep_exposed_code() {
    Fixture fixture;
    scenario.install_error_after_activation = true;
    scenario.destroy_failure = true;
    char error[512]{};
    void *original = nullptr;
    assert(fixture.manager.install_owned_hook(reinterpret_cast<void *>(0x1120),
                                              reinterpret_cast<void *>(0x8000), &original, error,
                                              sizeof(error)) != DEJAVU_OK);
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
    assert(fixture.manager.install_owned_hook(target, reinterpret_cast<void *>(0x8000), &original,
                                              error,
                                              sizeof(error)) == DEJAVU_ERROR_INVALID_ARGUMENT);
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
                                              reinterpret_cast<void *>(0x8000), &original, error,
                                              sizeof(error)) != DEJAVU_OK);
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
        assert(scenario.modules_destroyed == 0 && scenario.prepares == 0 &&
               scenario.shutdowns == 0);
        assert(error.find(fail_destroy ? "DobbyDestroy failed" : "permission restore failure") !=
               std::string::npos);
        scenario.destroy_failure = false;
        assert(fixture.manager.unload_plugin("first", &status, &error));
        assert(fixture.manager.plugins_.empty() && fixture.manager.hooks_.empty());
        assert(scenario.modules_destroyed == 1 && scenario.prepares == 0 &&
               scenario.shutdowns == 0);
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
void test_initializer_owner_does_not_require_loaded_state() {
    Fixture fixture;
    fixture.manager.plugins_[0]->loaded = false;
    fixture.manager.plugins_[0]->capabilities = DEJAVU_PLUGIN_CAP_STREAM;
    fixture.manager.plugins_[0]->stream_owner_token = 17;
    assert(fixture.manager.stream_owner_for_context() == 17);
}

void test_context_never_borrows_another_plugins_capability() {
    Fixture fixture;
    fixture.add_plugin("second");
    fixture.manager.plugins_[1]->capabilities = DEJAVU_PLUGIN_CAP_STREAM;
    fixture.manager.plugins_[1]->stream_owner_token = 23;
    assert(fixture.manager.stream_owner_for_context() == 0);
}

void stream_plugin(Fixture &fixture, size_t index, uint64_t token) {
    auto &plugin = fixture.manager.plugins_[index];
    plugin->capabilities = DEJAVU_PLUGIN_CAP_STREAM;
    plugin->stream_owner_token = token;
    fixture.manager.initialize_plugin_context(plugin.get());
}

void test_native_initializers_and_v1_callbacks_keep_their_identity() {
    Fixture fixture;
    fixture.add_plugin("second");
    stream_plugin(fixture, 0, 17);
    stream_plugin(fixture, 1, 23);
    fixture.manager.plugins_[0]->loaded = false;
    char error[128]{};
    void *first = fixture.manager.plugins_[0]->host.host_context;
    void *second = fixture.manager.plugins_[1]->host.host_context;
    assert(PluginManager::host_stream_listen(first, DEJAVU_STREAM_TRANSPORT_TCP, "127.0.0.1:1", 4,
                                             error, sizeof(error)) > 0);
    g_current_plugin_index = kPluginIndexNone;
    const int handle = PluginManager::host_stream_open(first, DEJAVU_STREAM_TRANSPORT_TCP,
                                                       "127.0.0.1:1", error, sizeof(error));
    assert(handle > 0 && stream_test_owners.at(handle) == 17);
    // v1 native capture callbacks use the saved instance host on arbitrary threads.
    std::thread callback([&] {
        assert(PluginManager::host_stream_write(first, handle, "frame", 5) == 0);
        assert(PluginManager::host_stream_write(second, handle, "bad", 3) == -1);
        assert(PluginManager::host_stream_close(second, handle) == -1);
    });
    callback.join();
    assert(PluginManager::host_stream_close(first, handle) == 0);
    assert(PluginManager::host_stream_listen(reinterpret_cast<void *>(0x1234),
                                             DEJAVU_STREAM_TRANSPORT_TCP, "127.0.0.1:1", 4, error,
                                             sizeof(error)) == 0);
    fixture.manager.plugins_[0]->loaded = true;
    g_current_plugin_index = 0;
}

void test_explicit_jit_workers_are_isolated_and_survive_other_plugin_removal() {
    Fixture fixture;
    fixture.add_plugin("second");
    stream_plugin(fixture, 0, 17);
    stream_plugin(fixture, 1, 23);
    void *first = dejavu_jit_stream_capture_context();
    g_current_plugin_index = 1;
    void *second = dejavu_jit_stream_capture_context();
    assert(first != nullptr && second != nullptr && first != second);
    char error[128]{};
    const int listener = dejavu_jit_stream_listen(DEJAVU_STREAM_TRANSPORT_TCP, "127.0.0.1:1", 4,
                                                  error, sizeof(error));
    g_current_plugin_index = kPluginIndexNone;
    std::thread worker([&] {
        assert(dejavu_jit_stream_accept(listener, 0) == -1);
        assert(dejavu_jit_stream_bind_context(second) == 0);
        const int accepted = dejavu_jit_stream_accept(listener, 0);
        assert(accepted > 0);
        char buffer[8]{};
        unsigned long size = 0;
        assert(dejavu_jit_stream_read(accepted, buffer, sizeof(buffer), 0, &size) == 1);
        assert(dejavu_jit_stream_write(accepted, buffer, size) == 0);
        assert(dejavu_jit_stream_bind_context(first) == 0);
        assert(dejavu_jit_stream_read(accepted, buffer, sizeof(buffer), 0, &size) == -1);
        assert(dejavu_jit_stream_write(accepted, "bad", 3) == -1);
        assert(dejavu_jit_stream_close(accepted) == -1);
        assert(dejavu_jit_stream_bind_context(second) == 0);
        assert(dejavu_jit_stream_close(accepted) == 0);
        assert(dejavu_jit_stream_bind_context(nullptr) == 0);
    });
    worker.join();
    g_current_plugin_index = 0;
    fixture.manager.plugins_[0]->capabilities = 0;
    assert(dejavu_jit_stream_capture_context() == nullptr);
    assert(dejavu_jit_stream_bind_context(second) == -1);
    std::string status, detail;
    assert(fixture.manager.unload_plugin("first", &status, &detail));
    g_current_plugin_index = kPluginIndexNone;
    std::thread surviving_worker([&] {
        assert(dejavu_jit_stream_bind_context(first) == -1);
        assert(dejavu_jit_stream_bind_context(second) == 0);
        assert(dejavu_jit_stream_accept(listener, 0) > 0);
        assert(dejavu_jit_stream_bind_context(nullptr) == 0);
    });
    surviving_worker.join();
}

std::thread lifecycle_worker;
std::mutex lifecycle_barrier_mutex;
std::condition_variable lifecycle_barrier;
bool lifecycle_go;
bool lifecycle_ready;
void prepare_and_join_worker() {
    {
        std::lock_guard<std::mutex> lock(lifecycle_barrier_mutex);
        lifecycle_go = true;
    }
    lifecycle_barrier.notify_one();
    lifecycle_worker.join();
}

void test_unload_and_shutdown_do_not_hold_manager_lock_while_joining() {
    for (bool shutdown : {false, true}) {
        Fixture fixture;
        stream_plugin(fixture, 0, 17);
        void *credential = dejavu_jit_stream_capture_context();
        char error[128]{};
        const int listener = dejavu_jit_stream_listen(DEJAVU_STREAM_TRANSPORT_TCP, "127.0.0.1:1", 4,
                                                      error, sizeof(error));
        lifecycle_go = false;
        lifecycle_ready = false;
        fixture.manager.plugins_[0]->jit_prepare = prepare_and_join_worker;
        lifecycle_worker = std::thread([&] {
            assert(dejavu_jit_stream_bind_context(credential) == 0);
            std::unique_lock<std::mutex> lock(lifecycle_barrier_mutex);
            lifecycle_ready = true;
            lifecycle_barrier.notify_one();
            lifecycle_barrier.wait(lock, [] { return lifecycle_go; });
            lock.unlock();
            assert(dejavu_jit_stream_accept(listener, -1) == -1);
            assert(dejavu_jit_stream_bind_context(nullptr) == 0);
        });
        {
            std::unique_lock<std::mutex> lock(lifecycle_barrier_mutex);
            lifecycle_barrier.wait(lock, [] { return lifecycle_ready; });
        }
        if (shutdown)
            fixture.manager.shutdown();
        else {
            std::string status, detail;
            assert(fixture.manager.unload_plugin("first", &status, &detail));
        }
        assert(fixture.manager.plugins_.empty());
        assert(dejavu_jit_stream_bind_context(credential) == -1);
    }
}

int callback_entered = 0;
int protected_callback(dejavu_control_session *, void *, const dejavu_value *, size_t,
                       dejavu_value *, char *, size_t) {
    ++callback_entered;
    return DEJAVU_OK;
}
void test_callback_admission_rejects_retired_entries() {
    Fixture fixture;
    auto entry = std::make_unique<PluginManager::Entry>();
    entry->manager = &fixture.manager;
    entry->valid = true;
    entry->function = protected_callback;
    auto *pointer = entry.get();
    fixture.manager.entries_.push_back(std::move(entry));
    char error[128]{};
    dejavu_value result{};
    assert(PluginManager::trampoline(nullptr, pointer, nullptr, 0, &result, error, sizeof(error)) ==
           DEJAVU_OK);
    assert(pointer->in_flight.load() == 0 && callback_entered == 1);
    std::string status, detail;
    assert(fixture.manager.unload_plugin("first", &status, &detail));
    assert(PluginManager::trampoline(nullptr, pointer, nullptr, 0, &result, error, sizeof(error)) !=
           DEJAVU_OK);
    assert(callback_entered == 1);
}

PluginManager *shift_manager;
PluginManager::Entry *shift_entry;
std::mutex shift_mutex;
std::condition_variable shift_condition;
bool shift_entered, shift_release;
uint64_t shifted_owner;
std::thread shift_worker;
int shifting_callback(dejavu_control_session *, void *, const dejavu_value *, size_t,
                      dejavu_value *, char *, size_t) {
    {
        std::unique_lock<std::mutex> lock(shift_mutex);
        shift_entered = true;
        shift_condition.notify_all();
        shift_condition.wait(lock, [] { return shift_release; });
    }
    shifted_owner = shift_manager->stream_owner_for_context();
    assert(dejavu_jit_stream_capture_context() == shift_manager->plugins_[0]->context.get());
    return DEJAVU_OK;
}
void prepare_while_another_plugin_enters_callback() {
    shift_worker = std::thread([] {
        char error[128]{};
        dejavu_value result{};
        assert(PluginManager::trampoline(nullptr, shift_entry, nullptr, 0, &result, error,
                                         sizeof(error)) == DEJAVU_OK);
    });
    std::unique_lock<std::mutex> lock(shift_mutex);
    shift_condition.wait(lock, [] { return shift_entered; });
}
void test_callback_identity_is_stable_across_another_plugins_unload() {
    Fixture fixture;
    fixture.add_plugin("second");
    fixture.add_plugin("third");
    stream_plugin(fixture, 0, 17);
    stream_plugin(fixture, 1, 23);
    stream_plugin(fixture, 2, 31);
    auto entry = std::make_unique<PluginManager::Entry>();
    entry->manager = &fixture.manager;
    entry->valid = true;
    entry->plugin_index = 1;
    entry->function = shifting_callback;
    shift_manager = &fixture.manager;
    shift_entry = entry.get();
    fixture.manager.entries_.push_back(std::move(entry));
    shift_entered = shift_release = false;
    shifted_owner = 0;
    fixture.manager.plugins_[0]->jit_prepare = prepare_while_another_plugin_enters_callback;
    std::string status, error;
    assert(fixture.manager.unload_plugin("first", &status, &error));
    {
        std::lock_guard<std::mutex> lock(shift_mutex);
        shift_release = true;
    }
    shift_condition.notify_all();
    shift_worker.join();
    assert(shifted_owner == 23 && fixture.manager.plugins_[0]->name == "second");
}

std::string native_loader_path;
bool native_loader_enabled = false;
bool metadata_checked = false, initializer_checked = false;
const DejavuPluginDescriptor *native_test_entry();
int nested_initializer(const DejavuPluginHost *host, void **, char *, size_t) {
    assert(active_plugin_index() == 1);
    assert(dejavu_jit_stream_capture_context() == nullptr);
    char error[128]{};
    assert(PluginManager::host_stream_listen(host->host_context, DEJAVU_STREAM_TRANSPORT_TCP,
                                             "127.0.0.1:1", 4, error, sizeof(error)) == 0);
    // A native initializer may await a worker using its instance host. The
    // worker's index resolution needs the manager mutex, so init must unlock it.
    std::thread worker([&] {
        auto *manager = PluginManager::from_host_context(host->host_context);
        assert(manager != nullptr && manager->index_for_context(host->host_context) == 1);
    });
    worker.join();
    initializer_checked = true;
    return 0;
}
const DejavuPluginDescriptor *native_test_entry() {
    assert(active_plugin_index() == kPluginIndexNone);
    assert(dejavu_jit_stream_capture_context() == nullptr);
    char error[128]{};
    assert(dejavu_jit_stream_listen(DEJAVU_STREAM_TRANSPORT_TCP, "127.0.0.1:1", 4, error,
                                    sizeof(error)) == 0);
    metadata_checked = true;
    static const DejavuPluginDescriptor descriptor = {
        DEJAVU_PLUGIN_ABI_VERSION,
        sizeof(DejavuPluginDescriptor),
        "nested_child",
        "1",
        DEJAVU_PLUGIN_FLAG_UNLOAD_SAFE,
        nested_initializer,
        nullptr,
        nullptr,
        0,
        nullptr,
    };
    return &descriptor;
}
int nested_loader_callback(dejavu_control_session *, void *, const dejavu_value *, size_t,
                           dejavu_value *, char *, size_t) {
    PluginManager *manager = g_jit_manager.load();
    void *original_context = dejavu_jit_stream_capture_context();
    assert(original_context != nullptr);
    std::string status, error;
    assert(manager->load_plugin(native_loader_path, &status, &error));
    assert(dejavu_jit_stream_capture_context() == original_context);
    assert(manager->stream_owner_for_context() == 17);
    return DEJAVU_OK;
}
void test_reentrant_load_masks_parent_identity_and_unlocks_initializers() {
    Fixture fixture;
    stream_plugin(fixture, 0, 17);
    char source_path[] = "/tmp/dejavu-plugin-context-XXXXXX";
    const int source_fd = mkstemp(source_path);
    assert(source_fd >= 0 && write(source_fd, "fixture", 7) == 7);
    close(source_fd);
    native_loader_path = source_path;
    native_loader_enabled = true;
    metadata_checked = initializer_checked = false;
    auto entry = std::make_unique<PluginManager::Entry>();
    entry->manager = &fixture.manager;
    entry->valid = true;
    entry->function = nested_loader_callback;
    auto *pointer = entry.get();
    fixture.manager.entries_.push_back(std::move(entry));
    char error[128]{};
    dejavu_value result{};
    assert(PluginManager::trampoline(nullptr, pointer, nullptr, 0, &result, error, sizeof(error)) ==
           DEJAVU_OK);
    assert(metadata_checked && initializer_checked && fixture.manager.plugins_.size() == 2);
    native_loader_enabled = false;
    assert(unlink(source_path) == 0);
}

unsigned int returned_leaf_calls = 0;
uint64_t returned_leaf_owner = 0;
int returned_leaf(dejavu_control_session *, void *context, const dejavu_value *, size_t,
                  dejavu_value *result, char *, size_t) {
    assert(context == &returned_leaf_calls);
    ++returned_leaf_calls;
    returned_leaf_owner = g_jit_manager.load()->stream_owner_for_context();
    result->type = DEJAVU_VALUE_INTEGER;
    result->integer_value = 42;
    return DEJAVU_OK;
}
int returned_factory(dejavu_control_session *, void *, const dejavu_value *, size_t,
                     dejavu_value *result, char *, size_t) {
    result->type = DEJAVU_VALUE_HOST_FUNCTION;
    result->host_function = returned_leaf;
    result->host_context = &returned_leaf_calls;
    return DEJAVU_OK;
}
PluginManager::Entry *callback_entry(Fixture &fixture, dejavu_host_function function,
                                     size_t plugin_index = 0) {
    auto entry = std::make_unique<PluginManager::Entry>();
    entry->manager = &fixture.manager;
    entry->valid = true;
    entry->plugin_index = plugin_index;
    entry->namespace_name = "factory";
    entry->function_name = "make";
    entry->function = function;
    auto *pointer = entry.get();
    fixture.manager.entries_.push_back(std::move(entry));
    return pointer;
}
void test_returned_host_function_is_scoped_and_revoked_on_unload() {
    Fixture fixture;
    stream_plugin(fixture, 0, 17);
    returned_leaf_calls = 0;
    returned_leaf_owner = 0;
    auto *entry = callback_entry(fixture, returned_factory);
    char error[256]{};
    dejavu_value closure{}, value{};
    assert(PluginManager::trampoline(nullptr, entry, nullptr, 0, &closure, error, sizeof(error)) ==
           DEJAVU_OK);
    assert(closure.type == DEJAVU_VALUE_HOST_FUNCTION);
    assert(closure.host_function != returned_leaf);
    // No ambient plugin identity: invoking the returned closure must restore it.
    g_current_plugin_index = kPluginIndexNone;
    assert(closure.host_function(nullptr, closure.host_context, nullptr, 0, &value, error,
                                 sizeof(error)) == DEJAVU_OK);
    assert(value.type == DEJAVU_VALUE_INTEGER && value.integer_value == 42);
    assert(returned_leaf_calls == 1 && returned_leaf_owner == 17);
    std::string status, unload_error;
    assert(fixture.manager.unload_plugin("first", &status, &unload_error));
    assert(closure.host_function(nullptr, closure.host_context, nullptr, 0, &value, error,
                                 sizeof(error)) != DEJAVU_OK);
    assert(returned_leaf_calls == 1);
}

int returned_nested_factory(dejavu_control_session *, void *, const dejavu_value *, size_t,
                            dejavu_value *result, char *, size_t) {
    result->type = DEJAVU_VALUE_HOST_FUNCTION;
    result->host_function = returned_factory;
    return DEJAVU_OK;
}
void test_returned_functions_are_recursive_and_deduplicated() {
    Fixture fixture;
    stream_plugin(fixture, 0, 17);
    auto *entry = callback_entry(fixture, returned_nested_factory);
    dejavu_value outer{}, leaf{}, value{};
    char error[256]{};
    assert(PluginManager::trampoline(nullptr, entry, nullptr, 0, &outer, error, sizeof(error)) ==
           DEJAVU_OK);
    assert(outer.host_function(nullptr, outer.host_context, nullptr, 0, &leaf, error,
                               sizeof(error)) == DEJAVU_OK);
    const size_t retained = fixture.manager.entries_.size();
    for (int i = 0; i < 4; ++i) {
        dejavu_value duplicate{};
        assert(outer.host_function(nullptr, outer.host_context, nullptr, 0, &duplicate, error,
                                   sizeof(error)) == DEJAVU_OK);
        assert(duplicate.host_context == leaf.host_context);
    }
    assert(fixture.manager.entries_.size() == retained);
    assert(leaf.host_function(nullptr, leaf.host_context, nullptr, 0, &value, error,
                              sizeof(error)) == DEJAVU_OK);
    assert(value.integer_value == 42);
    std::string status, unload_error;
    assert(fixture.manager.unload_plugin("first", &status, &unload_error));
    assert(outer.host_function(nullptr, outer.host_context, nullptr, 0, &value, error,
                               sizeof(error)) != DEJAVU_OK);
    assert(leaf.host_function(nullptr, leaf.host_context, nullptr, 0, &value, error,
                              sizeof(error)) != DEJAVU_OK);
}
int return_managed_value(dejavu_control_session *, void *context, const dejavu_value *, size_t,
                         dejavu_value *result, char *, size_t) {
    *result = *static_cast<dejavu_value *>(context);
    return DEJAVU_OK;
}
void test_already_managed_closures_are_not_rewrapped_or_named() {
    Fixture fixture;
    auto *entry = callback_entry(fixture, returned_factory);
    dejavu_value args[2]{}, closure{}, repeated{};
    args[0].type = args[1].type = DEJAVU_VALUE_STRING;
    args[0].string_value = "first";
    args[0].string_size = 5;
    args[1].string_value = "make";
    args[1].string_size = 4;
    char error[256]{};
    assert(PluginManager::lua_call(nullptr, &fixture.manager, args, 2, &closure, error,
                                   sizeof(error)) == DEJAVU_OK);
    entry->function = return_managed_value;
    entry->context = &closure;
    const size_t retained = fixture.manager.entries_.size();
    for (int i = 0; i < 4; ++i) {
        assert(PluginManager::trampoline(nullptr, entry, nullptr, 0, &repeated, error,
                                         sizeof(error)) == DEJAVU_OK);
        assert(repeated.host_function == closure.host_function &&
               repeated.host_context == closure.host_context);
    }
    assert(fixture.manager.entries_.size() == retained);
    args[1].string_value = "<returned host function>";
    args[1].string_size = 24;
    assert(PluginManager::lua_call(nullptr, &fixture.manager, args, 2, &repeated, error,
                                   sizeof(error)) != DEJAVU_OK);
    assert(fixture.manager.status().find("entries=1") != std::string::npos);
}
char borrowed_text[] = "module text";
int result_mode = 0;
int return_plain_value(dejavu_control_session *, void *, const dejavu_value *, size_t,
                       dejavu_value *result, char *, size_t) {
    if (result_mode == 0) {
        result->type = DEJAVU_VALUE_STRING;
        result->string_value = borrowed_text;
        result->string_size = sizeof(borrowed_text) - 1;
    } else if (result_mode == 1) {
        result->type = DEJAVU_VALUE_HANDLE;
        result->handle_value = borrowed_text;
    } else {
        result->type = DEJAVU_VALUE_STRING;
        result->owned = 1;
        result->string_value = strdup("owned text");
        result->string_size = 10;
    }
    return DEJAVU_OK;
}
void test_callback_result_values_keep_their_lifetimes() {
    Fixture fixture;
    auto *entry = callback_entry(fixture, return_plain_value);
    dejavu_value result{};
    char error[256]{};
    result_mode = 0;
    assert(PluginManager::trampoline(nullptr, entry, nullptr, 0, &result, error, sizeof(error)) ==
           DEJAVU_OK);
    assert(result.owned && result.string_value != borrowed_text &&
           std::string(result.string_value, result.string_size) == "module text");
    borrowed_text[0] = 'X';
    assert(result.string_value[0] == 'm');
    borrowed_text[0] = 'm';
    release_result(&result);
    result_mode = 1;
    assert(PluginManager::trampoline(nullptr, entry, nullptr, 0, &result, error, sizeof(error)) ==
           DEJAVU_OK);
    assert(result.type == DEJAVU_VALUE_HANDLE && result.handle_value == borrowed_text &&
           !result.owned);
    result_mode = 2;
    assert(PluginManager::trampoline(nullptr, entry, nullptr, 0, &result, error, sizeof(error)) ==
           DEJAVU_OK);
    assert(result.owned && std::string(result.string_value, result.string_size) == "owned text");
    release_result(&result);
}
int failure_mode = 0;
int failing_result(dejavu_control_session *, void *, const dejavu_value *, size_t,
                   dejavu_value *result, char *, size_t) {
    if (failure_mode == 2) {
        result->type = DEJAVU_VALUE_FUNCTION;
        result->function_value = 77;
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    result->type = DEJAVU_VALUE_STRING;
    result->owned = 1;
    result->string_value = strdup("release after failure");
    result->string_size = 21;
    if (failure_mode == 1)
        throw std::runtime_error("fixture failure");
    return DEJAVU_ERROR_LUA_EXECUTE;
}
void test_failed_and_throwing_callback_results_are_released() {
    Fixture fixture;
    auto *entry = callback_entry(fixture, failing_result);
    char error[256]{};
    dejavu_value value{};
    for (failure_mode = 0; failure_mode < 3; ++failure_mode) {
        error[0] = 0;
        assert(PluginManager::trampoline(reinterpret_cast<dejavu_control_session *>(1), entry,
                                         nullptr, 0, &value, error, sizeof(error)) != DEJAVU_OK);
        assert(value.type == DEJAVU_VALUE_NIL && entry->in_flight.load() == 0);
        if (failure_mode == 1)
            assert(std::string(error).find("fixture failure") != std::string::npos);
    }
    assert(scenario.result_releases == 3 && scenario.function_releases == 1);
}
std::mutex closure_mutex;
std::condition_variable closure_condition;
bool closure_entered = false, closure_release = false;
int blocking_returned_leaf(dejavu_control_session *, void *, const dejavu_value *, size_t,
                           dejavu_value *, char *, size_t) {
    std::unique_lock<std::mutex> lock(closure_mutex);
    closure_entered = true;
    closure_condition.notify_all();
    closure_condition.wait(lock, [] { return closure_release; });
    return DEJAVU_OK;
}
void test_returned_callback_inflight_prevents_unload() {
    Fixture fixture;
    dejavu_value raw{}, managed{};
    raw.type = DEJAVU_VALUE_HOST_FUNCTION;
    raw.host_function = blocking_returned_leaf;
    auto *entry = callback_entry(fixture, return_managed_value);
    entry->context = &raw;
    char error[256]{};
    assert(PluginManager::trampoline(nullptr, entry, nullptr, 0, &managed, error, sizeof(error)) ==
           DEJAVU_OK);
    closure_entered = closure_release = false;
    std::thread worker([&] {
        dejavu_value value{};
        char call_error[256]{};
        assert(managed.host_function(nullptr, managed.host_context, nullptr, 0, &value, call_error,
                                     sizeof(call_error)) == DEJAVU_OK);
    });
    {
        std::unique_lock<std::mutex> lock(closure_mutex);
        closure_condition.wait(lock, [] { return closure_entered; });
    }
    std::string status, unload_error;
    assert(!fixture.manager.unload_plugin("first", &status, &unload_error));
    assert(unload_error.find("still executing") != std::string::npos &&
           scenario.modules_destroyed == 0);
    {
        std::lock_guard<std::mutex> lock(closure_mutex);
        closure_release = true;
    }
    closure_condition.notify_all();
    worker.join();
    assert(fixture.manager.unload_plugin("first", &status, &unload_error));
    assert(scenario.modules_destroyed == 1);
}
void test_invalid_or_failed_function_results_never_publish_bindings() {
    Fixture fixture;
    dejavu_value raw{}, value{};
    raw.type = DEJAVU_VALUE_HOST_FUNCTION;
    auto *entry = callback_entry(fixture, return_managed_value);
    entry->context = &raw;
    char error[256]{};
    for (bool spoofed : {false, true}) {
        raw.host_function = spoofed ? &PluginManager::trampoline : nullptr;
        raw.host_context = reinterpret_cast<void *>(0x123);
        const size_t before = fixture.manager.entries_.size();
        assert(PluginManager::trampoline(nullptr, entry, nullptr, 0, &value, error,
                                         sizeof(error)) != DEJAVU_OK);
        assert(value.type == DEJAVU_VALUE_NIL && fixture.manager.entries_.size() == before);
    }
}
void test_returned_closure_survives_slot_shift_but_not_reload() {
    Fixture fixture;
    fixture.add_plugin("second");
    stream_plugin(fixture, 0, 17);
    stream_plugin(fixture, 1, 23);
    auto *entry = callback_entry(fixture, returned_factory, 1);
    dejavu_value closure{}, value{};
    char error[256]{};
    assert(PluginManager::trampoline(nullptr, entry, nullptr, 0, &closure, error, sizeof(error)) ==
           DEJAVU_OK);
    std::string status, unload_error;
    assert(fixture.manager.unload_plugin("first", &status, &unload_error));
    g_current_plugin_index = kPluginIndexNone;
    assert(closure.host_function(nullptr, closure.host_context, nullptr, 0, &value, error,
                                 sizeof(error)) == DEJAVU_OK);
    assert(returned_leaf_owner == 23);
    assert(fixture.manager.unload_plugin("second", &status, &unload_error));
    fixture.add_plugin("second");
    stream_plugin(fixture, 0, 31);
    assert(closure.host_function(nullptr, closure.host_context, nullptr, 0, &value, error,
                                 sizeof(error)) != DEJAVU_OK);
}

} // namespace

void *plugin_test_dlsym(void *, const char *name) {
    return native_loader_enabled && std::strcmp(name, DEJAVU_PLUGIN_ENTRY_SYMBOL) == 0
               ? reinterpret_cast<void *>(native_test_entry)
               : nullptr;
}
int plugin_test_dlclose(void *) {
    return 0;
}
extern "C" void *android_dlopen_ext(const char *, int, const android_dlextinfo *) {
    assert(native_loader_enabled);
    return reinterpret_cast<void *>(0x5500);
}
extern "C" int __system_property_get(const char *name, char *value) {
    if (native_loader_enabled && std::strcmp(name, "debug.dejavu.plugins.allowed") == 0) {
        assert(native_loader_path.size() < PROP_VALUE_MAX);
        std::memcpy(value, native_loader_path.c_str(), native_loader_path.size() + 1);
        return static_cast<int>(native_loader_path.size());
    }
    value[0] = 0;
    return 0;
}

extern "C" int __android_log_print(int, const char *, const char *, ...) {
    return 0;
}
extern "C" int __android_log_write(int, const char *, const char *) {
    return 0;
}
extern "C" int DobbyIsHooked(void *target) {
    return scenario.installed.count(target) ? 1 : 0;
}
extern "C" int DobbyHook(void *target, void *, void **original) {
    *original = reinterpret_cast<void *>(0x7000);
    scenario.installed.insert(target);
    return scenario.install_error_after_activation ? -1 : 0;
}
extern "C" int DobbyDestroy(void *target) {
    ++scenario.destroys;
    if (scenario.destroy_failure)
        return -1;
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
    if (!scenario.apply_failure)
        return true;
    *error = "injected permission apply failure";
    return false;
}
bool root_helper_restore(const RootHelperProtectionSnapshot &snapshot, std::string *error) {
    ++scenario.restores;
    assert(snapshot.ranges.size() == 1 && snapshot.ranges[0].length == 0x1000);
    assert(snapshot.ranges[0].protection == PROT_EXEC);
    if (scenario.restore_failures == 0)
        return true;
    --scenario.restore_failures;
    *error = "injected permission restore failure";
    return false;
}

StreamService::StreamService() = default;
StreamService::~StreamService() = default;
int StreamService::open(uint64_t owner, uint32_t, const std::string &, std::string *) {
    std::lock_guard<std::mutex> lock(stream_test_mutex);
    if (stream_test_retired.count(owner))
        return 0;
    const int handle = stream_test_next++;
    stream_test_owners[handle] = owner;
    return handle;
}
int StreamService::listen(uint64_t owner, uint32_t transport, const std::string &endpoint, int,
                          std::string *error) {
    return open(owner, transport, endpoint, error);
}
int StreamService::accept(int handle, int, uint64_t owner) {
    std::lock_guard<std::mutex> lock(stream_test_mutex);
    if (!owned_by(handle, owner))
        return -1;
    const int accepted = stream_test_next++;
    stream_test_owners[accepted] = owner;
    return accepted;
}
int StreamService::read(int handle, void *buffer, size_t capacity, int, size_t *size,
                        uint64_t owner) {
    std::lock_guard<std::mutex> lock(stream_test_mutex);
    if (!owned_by(handle, owner) || capacity == 0)
        return -1;
    static_cast<char *>(buffer)[0] = 'x';
    *size = 1;
    return 1;
}
int StreamService::write(int handle, const void *, size_t, uint64_t owner) {
    std::lock_guard<std::mutex> lock(stream_test_mutex);
    return owned_by(handle, owner) ? 0 : -1;
}
int StreamService::close(int handle, uint64_t owner) {
    std::lock_guard<std::mutex> lock(stream_test_mutex);
    return owned_by(handle, owner) && stream_test_owners.erase(handle) != 0 ? 0 : -1;
}
bool StreamService::owned_by(int handle, uint64_t owner) const {
    const auto found = stream_test_owners.find(handle);
    return found != stream_test_owners.end() && found->second == owner;
}
size_t StreamService::close_owner(uint64_t owner) {
    std::lock_guard<std::mutex> lock(stream_test_mutex);
    size_t count = 0;
    for (auto entry = stream_test_owners.begin(); entry != stream_test_owners.end();) {
        if (entry->second == owner) {
            entry = stream_test_owners.erase(entry);
            ++count;
        } else
            ++entry;
    }
    return count;
}
size_t StreamService::retire_owner(uint64_t owner) {
    {
        std::lock_guard<std::mutex> lock(stream_test_mutex);
        stream_test_retired.insert(owner);
    }
    return close_owner(owner);
}

int main() {
    test_returned_host_function_is_scoped_and_revoked_on_unload();
    test_returned_functions_are_recursive_and_deduplicated();
    test_already_managed_closures_are_not_rewrapped_or_named();
    test_callback_result_values_keep_their_lifetimes();
    test_failed_and_throwing_callback_results_are_released();
    test_returned_callback_inflight_prevents_unload();
    test_invalid_or_failed_function_results_never_publish_bindings();
    test_returned_closure_survives_slot_shift_but_not_reload();
    test_callback_identity_is_stable_across_another_plugins_unload();
    test_reentrant_load_masks_parent_identity_and_unlocks_initializers();
    test_native_initializers_and_v1_callbacks_keep_their_identity();
    test_explicit_jit_workers_are_isolated_and_survive_other_plugin_removal();
    test_unload_and_shutdown_do_not_hold_manager_lock_while_joining();
    test_callback_admission_rejects_retired_entries();
    test_initializer_owner_does_not_require_loaded_state();
    test_context_never_borrows_another_plugins_capability();
    test_destroy_failure_retains_module_until_retry();
    test_restore_failure_retries_without_destroying_twice();
    test_failed_install_and_shutdown_keep_exposed_code();
    test_foreign_hook_is_not_removed();
    test_failed_initializer_cleanup_and_index_shift();
    test_unpatched_permission_cleanup_remains_owned();
    test_failed_initializer_without_unload_safe_retries_host_cleanup();
    test_initialized_unsafe_plugin_keeps_unload_restriction();
    std::puts("OK: 24 plugin lifecycle groups: native/JIT ownership, callback identity, worker "
              "joins and retryable hook cleanup");
}
