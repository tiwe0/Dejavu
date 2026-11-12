#include "native_hook_manager.h"
#include "root_helper_client.h"
#include <dobby.h>
#include <dobby/dejavu_hook_diagnostics.h>
#include <dobby/dejavu_hook_mutation.h>

#include <sys/mman.h>
#include <unistd.h>
#include <dlfcn.h>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct dejavu_module {
    unsigned int ordinal;
};

namespace {

[[noreturn]] void fail(const std::string &message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}
void expect(bool condition, const std::string &message) {
    if (!condition)
        fail(message);
}

struct Scenario {
    bool compile_failure = false;
    bool missing_entry = false;
    bool prepare_failure = false;
    bool foreign_hook_races_prepare = false;
    bool activated_failure = false;
    bool destroy_failure = false;
    bool capture_failure = false;
    bool apply_failure = false;
    bool restore_failure = false;
    bool installed = false;
    bool writable = false;
    unsigned int compiled = 0;
    unsigned int destroyed = 0;
    unsigned int captures = 0;
    unsigned int applies = 0;
    unsigned int restores = 0;
    unsigned int activations = 0;
    unsigned int destroy_calls = 0;
    unsigned int observations_before_return = 0;
    unsigned long long context = 0;
    int mapping_protection = PROT_EXEC;
    bool anonymous_mapping = false;
    bool pin_unavailable = false;
    const char *backend_failure_stage = nullptr;
    bool overwrite_stage_on_restore = false;
    bool mutation_rejected = false;
    unsigned int stage_queries = 0;
    unsigned int identified_images = 0;
    unsigned int pin_attempts = 0;
    std::string source;
} scenario;
std::string library;
void *cold_target = nullptr;
std::vector<dejavu_module *> modules;
std::unordered_map<std::string, const void *> helper_symbols;
std::unordered_map<std::string, dejavu_host_function> lua_functions;
bool address_mode = false;
thread_local const char *last_backend_failure_stage = nullptr;

int register_function(dejavu_control_session *, const char *name_space, const char *name,
                      dejavu_host_function function, void *, char *, size_t) {
    expect(std::strcmp(name_space, "native") == 0, "register in native namespace");
    lua_functions[name] = function;
    return DEJAVU_OK;
}
dejavu_value text_value(const std::string &text) {
    dejavu_value value{};
    value.type = DEJAVU_VALUE_STRING;
    value.string_value = text.data(); value.string_size = text.size();
    return value;
}
dejavu_value address_value() {
    dejavu_value value{};
    value.type = DEJAVU_VALUE_INTEGER;
    value.integer_value = static_cast<long long>(reinterpret_cast<uintptr_t>(cold_target));
    return value;
}

int compile(const dejavu_engine *, const char *source, dejavu_module **out, char *error,
            size_t error_size) {
    scenario.source = source;
    ++scenario.compiled;
    if (scenario.compile_failure) {
        *out = nullptr;
        std::snprintf(error, error_size, "injected compile failure");
        return DEJAVU_ERROR_TCC_COMPILE;
    }
    *out = new dejavu_module{scenario.compiled};
    modules.push_back(*out);
    return DEJAVU_OK;
}
int add_symbol(dejavu_engine *, const char *name, const void *address) {
    helper_symbols[name] = address;
    return DEJAVU_OK;
}
void destroy_module(dejavu_module *) {
    ++scenario.destroyed;
}
void replacement_placeholder() {
}
void *module_symbol(dejavu_module *, const char *) {
    return scenario.missing_entry ? nullptr : reinterpret_cast<void *>(&replacement_placeholder);
}
DejavuRuntime runtime() {
    DejavuRuntime value{};
    value.engine_add_symbol = add_symbol;
    value.engine_compile = compile;
    value.module_symbol = module_symbol;
    value.module_destroy = destroy_module;
    value.control_session_register = register_function;
    return value;
}

void check_original(unsigned long long context) {
    const auto found = helper_symbols.find("dejavu_native_original_context");
    expect(found != helper_symbols.end(), "runtime must register the native context ABI helper");
    const auto helper =
        reinterpret_cast<void *(*)(unsigned long long)>(const_cast<void *>(found->second));
    void *original = helper(context);
    expect(original == NativeHookManager::original_for_context(context),
           "registered C ABI helper must resolve the same stable callback context");
    expect(original == cold_target,
           "replacement must see prepared original before backend returns");
    expect(reinterpret_cast<int (*)(int)>(original)(35) == 42,
           "prepared original must remain callable");
}
} // namespace

// The production class grants this fixture access only to its control operations.
struct NativeHookManagerTest {
    NativeHookManager manager;
    DejavuRuntime api = runtime();
    NativeHookManagerTest() {
        scenario = {};
        helper_symbols.clear();
        lua_functions.clear();
        std::string error;
        expect(manager.initialize(&api, reinterpret_cast<dejavu_engine *>(this), &error), error);
        expect(manager.register_lua(reinterpret_cast<dejavu_control_session *>(this), &error), error);
    }
    int invoke(const char *name, const std::vector<dejavu_value> &arguments,
               dejavu_value *result, std::string *error) {
        const auto found = lua_functions.find(name);
        expect(found != lua_functions.end(), std::string("missing native.") + name);
        char buffer[512]{};
        const int status = found->second(nullptr, &manager, arguments.data(), arguments.size(),
                                          result, buffer, sizeof(buffer));
        *error = buffer;
        return status;
    }
    bool install(unsigned long long *id, std::string *error) {
        if (address_mode) {
            const std::string source = "int dejavu_native_target(int value) { return value; }";
            dejavu_value result{};
            const int status = invoke("install_address", {address_value(), text_value(source)}, &result, error);
            if (status == DEJAVU_OK) *id = result.integer_value;
            return status == DEJAVU_OK;
        }
        return manager.install(library, "dejavu_test_cold_target",
                               "int dejavu_native_target(int value) { return value; }", id, error);
    }
    bool uninstall(unsigned long long id, std::string *error) {
        return manager.uninstall(id, error);
    }
    std::string list() {
        return manager.list();
    }
    unsigned long long retained_id(unsigned long long returned) {
        if (returned != 0)
            return returned;
        auto text = list();
        expect(!text.empty(), "failed exposed install must leave a discoverable record");
        return std::stoull(text);
    }
    void check_callback_while_control_lock_is_held(unsigned long long context,
                                                   unsigned long long id) {
        std::lock_guard<std::mutex> lock(manager.mutex_);
        check_original(context);
        NativeHookManager::hit_for_context(context);
        expect(NativeHookManager::original_for_id(id) == cold_target,
               "compatibility lookup must not acquire the control mutex");
        NativeHookManager::hit_for_id(id);
    }
};

FILE *native_test_fopen(const char *path, const char *mode) {
    if (std::strcmp(path, "/proc/self/maps") != 0)
        return std::fopen(path, mode);
    FILE *file = std::tmpfile();
    expect(file != nullptr, "create synthetic maps stream");
    const uintptr_t page_size = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    const uintptr_t address = reinterpret_cast<uintptr_t>(cold_target);
    const uintptr_t start = address - address % page_size;
    const char *permissions = scenario.mapping_protection == PROT_EXEC ? "--xp" : "r--p";
    std::fprintf(file, "%lx-%lx %s 00000000 00:00 0 %s\n", static_cast<unsigned long>(start),
                 static_cast<unsigned long>(start + page_size), permissions, library.c_str());
    std::rewind(file);
    return file;
}

int native_test_dladdr(const void *address, Dl_info *information) {
    if (scenario.anonymous_mapping) return 0;
    const int found = dladdr(address, information);
    if (found != 0) ++scenario.identified_images;
    return found;
}

void *native_test_dlopen(const char *filename, int flags) {
    if ((flags & RTLD_NOLOAD) != 0) {
        ++scenario.pin_attempts;
        if (scenario.pin_unavailable) return nullptr;
    }
    return dlopen(filename, flags);
}

extern "C" int __android_log_print(int, const char *, const char *, ...) {
    return 0;
}
extern "C" int __android_log_write(int, const char *, const char *) {
    return 0;
}
extern "C" int DobbyHook(void *, void *, void **) {
    fail("manager must use pre-publication backend");
}
extern "C" int DobbyHookWithPrepare(void *, void *, dobby_hook_prepare_t prepare, void *opaque) {
    if (scenario.mutation_rejected) {
        last_backend_failure_stage = "outer-operation-stage";
        return dejavu_dobby::kHookMutationReentrant;
    }
    last_backend_failure_stage = nullptr;
    if (scenario.prepare_failure) {
        last_backend_failure_stage = scenario.backend_failure_stage;
        if (scenario.foreign_hook_races_prepare)
            scenario.installed = true;
        return -1;
    }
    scenario.context = reinterpret_cast<unsigned long long>(opaque);
    prepare(cold_target, 4, opaque);
    check_original(scenario.context);
    auto hit = helper_symbols.find("dejavu_native_hit_context");
    expect(hit != helper_symbols.end(), "runtime must register the native hit ABI helper");
    reinterpret_cast<void (*)(unsigned long long)>(const_cast<void *>(hit->second))(
        scenario.context);
    ++scenario.observations_before_return;
    scenario.installed = true;
    ++scenario.activations;
    if (scenario.activated_failure)
        last_backend_failure_stage = scenario.backend_failure_stage;
    return scenario.activated_failure ? -1 : 0;
}
const char *dejavu_dobby::LastHookFailureStage() {
    ++scenario.stage_queries;
    return last_backend_failure_stage;
}
extern "C" int DobbyDestroy(void *) {
    ++scenario.destroy_calls;
    if (scenario.destroy_failure || !scenario.installed)
        return -1;
    scenario.installed = false;
    return 0;
}
extern "C" int DobbyIsHooked(void *) {
    return scenario.installed ? 1 : 0;
}

bool root_helper_available() {
    return true;
}
bool root_helper_mapping(void *address, uintptr_t *start, size_t *length, int *protection,
                         std::string *) {
    const auto page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    *start = reinterpret_cast<uintptr_t>(address) / page * page;
    *length = page;
    *protection = PROT_EXEC;
    return true;
}
bool root_helper_capture(void *address, size_t size, RootHelperProtectionSnapshot *snapshot,
                         std::string *error) {
    ++scenario.captures;
    if (scenario.capture_failure) {
        *error = "injected capture failure";
        return false;
    }
    expect(size == 4, "native capture must cover exactly the single ARM64 entry instruction");
    uintptr_t start;
    size_t length;
    int protection;
    root_helper_mapping(address, &start, &length, &protection, error);
    snapshot->ranges = {{start, length, protection}};
    return true;
}
bool root_helper_apply_read_write(const RootHelperProtectionSnapshot &, std::string *error) {
    ++scenario.applies;
    scenario.writable = true;
    if (scenario.apply_failure) {
        *error = "injected apply failure";
        return false;
    }
    return true;
}
bool root_helper_restore(const RootHelperProtectionSnapshot &, std::string *error) {
    ++scenario.restores;
    if (scenario.overwrite_stage_on_restore)
        last_backend_failure_stage = "cleanup-overwrote-stage";
    if (scenario.restore_failure) {
        *error = "injected restore failure";
        return false;
    }
    scenario.writable = false;
    return true;
}
bool root_helper_unlock(void *, int *protection, size_t *length, std::string *) {
    ++scenario.applies;
    scenario.writable = true;
    *protection = PROT_EXEC;
    *length = 4096;
    return true;
}
bool root_helper_restore(void *, size_t, int, std::string *) {
    ++scenario.restores;
    scenario.writable = false;
    return true;
}

namespace {
void caller_owned_address_targets() {
    const std::string source = "int dejavu_native_target(int value) { return value; }";
    char path[] = "/tmp/dejavu-caller-owned-source.XXXXXX";
    const int descriptor = mkstemp(path);
    expect(descriptor >= 0, "create caller-owned address source fixture");
    expect(write(descriptor, source.data(), source.size()) == static_cast<ssize_t>(source.size()),
           "write caller-owned replacement source");
    close(descriptor);
    const std::string path_text = path;
    for (bool from_file : {false, true}) {
        for (bool anonymous : {false, true}) {
            NativeHookManagerTest test;
            scenario.pin_unavailable = !anonymous;
            scenario.anonymous_mapping = anonymous;
            const char *entry = from_file ? "install_address_file" : "install_address";
            const auto source_argument = text_value(from_file ? path_text : source);
            dejavu_value result{}; std::string error;
            auto unmapped = address_value(); unmapped.integer_value = 4;
            expect(test.invoke(entry, {unmapped, source_argument}, &result, &error) != DEJAVU_OK,
                   "caller-owned path still rejects unmapped addresses");
            scenario.mapping_protection = PROT_READ;
            expect(test.invoke(entry, {address_value(), source_argument}, &result, &error) != DEJAVU_OK,
                   "caller-owned path still rejects non-executable addresses");
            expect(scenario.compiled == 0 && scenario.captures == 0 && scenario.applies == 0 &&
                       scenario.identified_images == 0 && scenario.pin_attempts == 0,
                   "invalid mappings must fail before compilation, protection or linker lookups");
            scenario.mapping_protection = PROT_EXEC;
            expect(test.invoke(entry, {address_value(), source_argument}, &result, &error) == DEJAVU_OK,
                   "valid caller-owned target must install without a module reference: " + error);
            const unsigned long long id = result.integer_value;
            expect(scenario.identified_images == (anonymous ? 0U : 1U) &&
                       scenario.pin_attempts == (anonymous ? 0U : 1U),
                   "namespace fixture must identify the real image but fail RTLD_NOLOAD only");
            expect(test.list().find(" hits=1 lifetime=caller-owned") != std::string::npos,
                   "native.list must expose caller-owned lifetime after the existing fields");
            if (anonymous)
                expect(test.list().find("<address>#") != std::string::npos,
                       "anonymous mapping must identify its raw target");
            else {
                Dl_info identified{};
                expect(dladdr(cold_target, &identified) != 0 && identified.dli_fname != nullptr,
                       "namespace fixture must still have a real linker image");
                expect(test.list().find(std::string(identified.dli_fname) + "#") != std::string::npos,
                       "namespace-invisible image must preserve its identified module name");
            }
            check_original(scenario.context);
            NativeHookManager::hit_for_context(scenario.context);
            dejavu_value hook_id{};
            hook_id.type = DEJAVU_VALUE_INTEGER;
            hook_id.integer_value = static_cast<long long>(id);
            expect(test.invoke("stats", {hook_id}, &result, &error) == DEJAVU_OK && result.integer_value == 2,
                   "caller-owned replacement must retain callable original and hit accounting");
            expect(test.uninstall(id, &error), error);
            expect(!scenario.installed && !scenario.writable && scenario.destroy_calls == 1,
                   "caller-owned uninstall must restore the entry and saved protection");
            check_original(scenario.context);
            test.manager.shutdown();
            check_original(scenario.context);
            expect(scenario.destroyed == 0,
                   "entered caller-owned contexts and replacement code remain retained");
        }
    }
    unlink(path);
}

void isolated_address_module_lifetime(void *handle) {
    // This runs in its own process before any published hook can pin the fixture.
    dlclose(handle);
    void *probe = dlopen(library.c_str(), RTLD_NOW | RTLD_NOLOAD);
    expect(probe == nullptr, "lifetime fixture must actually unload without framework references");
    handle = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    expect(handle != nullptr, "reload lifetime fixture");
    cold_target = dlsym(handle, "dejavu_test_cold_target");
    {
        NativeHookManagerTest test;
        scenario.compile_failure = true;
        address_mode = true;
        unsigned long long id = 0; std::string error;
        expect(!test.install(&id, &error), "injected address compile failure");
    }
    dlclose(handle);
    probe = dlopen(library.c_str(), RTLD_NOW | RTLD_NOLOAD);
    expect(probe == nullptr, "unpublished failed install must release its module reference");
    handle = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    expect(handle != nullptr, "reload for successful address installation");
    cold_target = dlsym(handle, "dejavu_test_cold_target");
    unsigned long long context = 0;
    {
        NativeHookManagerTest test;
        unsigned long long id = 0; std::string error;
        expect(test.install(&id, &error), error);
        context = scenario.context;
        dlclose(handle); // The framework is now the only owner of the loaded image.
        probe = dlopen(library.c_str(), RTLD_NOW | RTLD_NOLOAD);
        expect(probe != nullptr, "address installation must keep the target image loaded");
        dlclose(probe);
        check_original(context);
        expect(test.uninstall(id, &error), error);
        check_original(context);
        test.manager.shutdown();
    }
    check_original(context);
    std::cout << "OK: isolated unloadable image: failed install releases pin; original survives external dlclose, uninstall and manager teardown\n";
}

void address_registration_and_validation() {
    NativeHookManagerTest test;
    expect(lua_functions.count("install_address") && lua_functions.count("install_address_file"),
           "both address API variants must be registered");
    const std::string source = "int dejavu_native_target(int value) { return value; }";
    std::vector<dejavu_value> invalid(7);
    invalid[1].type = DEJAVU_VALUE_INTEGER; invalid[1].integer_value = 0;
    invalid[2].type = DEJAVU_VALUE_INTEGER; invalid[2].integer_value = -1;
    invalid[3].type = DEJAVU_VALUE_NUMBER; invalid[3].number_value = 4.0;
    invalid[4].type = DEJAVU_VALUE_NUMBER;
    invalid[4].number_value = std::numeric_limits<double>::infinity();
    invalid[5].type = DEJAVU_VALUE_BOOLEAN; invalid[5].boolean_value = 1;
    invalid[6].type = DEJAVU_VALUE_HANDLE; invalid[6].handle_value = cold_target;
    dejavu_value result{}; std::string error;
    for (const auto &address : invalid) {
        expect(test.invoke("install_address", {address, text_value(source)}, &result, &error) == DEJAVU_ERROR_INVALID_ARGUMENT,
               "invalid address types must be rejected before touching code");
    }
    const std::vector<std::string> bad_text = {"", "0x", "0x0", "-0x4", "+0x4", " 0x4", "0x4 ",
        "1234", "0xg", "0x1;return true", "0x10000000000000000", std::string("0x4\0bad", 7)};
    for (const auto &text : bad_text) {
        expect(test.invoke("install_address", {text_value(text), text_value(source)}, &result, &error) == DEJAVU_ERROR_INVALID_ARGUMENT,
               "malformed/overflow address strings must be rejected");
    }
    expect(test.invoke("install_address", {address_value()}, &result, &error) == DEJAVU_ERROR_INVALID_ARGUMENT,
           "address installation requires source");
    auto unaligned = address_value(); ++unaligned.integer_value;
    expect(test.invoke("install_address", {unaligned, text_value(source)}, &result, &error) != DEJAVU_OK,
           "arm64 target address must be four-byte aligned");
    auto unmapped = address_value(); unmapped.integer_value = 4;
    expect(test.invoke("install_address", {unmapped, text_value(source)}, &result, &error) != DEJAVU_OK,
           "unmapped address must fail before backend admission");
    scenario.mapping_protection = PROT_READ;
    expect(test.invoke("install_address", {address_value(), text_value(source)}, &result, &error) != DEJAVU_OK,
           "non-executable addresses must fail before backend admission");
    expect(scenario.compiled == 0 && scenario.captures == 0 && scenario.applies == 0,
           "invalid addresses must not compile or change memory permissions");
}
void address_hex_file_and_shared_duplicate_guard() {
    NativeHookManagerTest test;
    char address[40];
    std::snprintf(address, sizeof(address), "0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(cold_target)));
    char path[] = "/tmp/dejavu-address-source.XXXXXX";
    const int descriptor = mkstemp(path);
    expect(descriptor >= 0, "create address source fixture");
    const std::string source = "int dejavu_native_target(int value) { return value; }";
    expect(write(descriptor, source.data(), source.size()) == static_cast<ssize_t>(source.size()), "write replacement source");
    close(descriptor);
    const std::string path_text = path, address_text = address;
    dejavu_value result{}; std::string error;
    expect(test.invoke("install_address_file", {text_value(address_text), text_value(path_text)}, &result, &error) == DEJAVU_OK,
           "hex string target and device source must install");
    unlink(path);
    const unsigned long long id = result.integer_value;
    expect(id > 0 && scenario.source.find(source) != std::string::npos, "file variant must compile the supplied replacement");
    const std::string symbol = "dejavu_test_cold_target";
    expect(test.invoke("install", {text_value(library), text_value(symbol), text_value(source)}, &result, &error) != DEJAVU_OK,
           "symbol entry must reject a target already installed by address");
    expect(scenario.compiled == 1 && scenario.applies == 1, "duplicate guard is shared by symbol and address paths");
    expect(test.list().find(address_text) != std::string::npos, "address installs must identify their target in native.list");
    expect(test.uninstall(id, &error), error);
    expect(test.invoke("install_address_file", {text_value(address_text), text_value(path_text)}, &result, &error) == DEJAVU_ERROR_INVALID_ARGUMENT,
           "missing source file must not install a hook");
}
void compile_failure_never_changes_permissions() {
    NativeHookManagerTest test;
    scenario.compile_failure = true;
    unsigned long long id = 0;
    std::string error;
    expect(!test.install(&id, &error), "compile error must fail install");
    expect(error.find("compile") != std::string::npos, "compile error must be reported");
    expect(scenario.applies == 0 && !scenario.writable,
           "compile failure must precede permission changes");
    expect(scenario.activations == 0, "compile failure must not patch entry");
}
void missing_entry_never_changes_permissions() {
    NativeHookManagerTest test;
    scenario.missing_entry = true;
    unsigned long long id = 0;
    std::string error;
    expect(!test.install(&id, &error), "missing entry must fail install");
    expect(scenario.applies == 0 && !scenario.writable,
           "missing entry must precede permission changes");
    expect(scenario.destroyed == 1, "unpublished compiled module must be reclaimed");
}
void capture_failure_never_compiles_or_patches() {
    NativeHookManagerTest test;
    scenario.capture_failure = true;
    unsigned long long id = 0;
    std::string error;
    expect(!test.install(&id, &error), "capture error must fail install");
    expect(scenario.compiled == 0 && scenario.applies == 0 && scenario.activations == 0,
           "invalid mapping must be rejected before compilation or permission changes");
}
void apply_failure_restores_permissions_before_returning() {
    NativeHookManagerTest test;
    scenario.apply_failure = true;
    unsigned long long id = 0;
    std::string error;
    expect(!test.install(&id, &error), "permission change failure must fail install");
    expect(scenario.activations == 0, "permission change failure must not publish an entry patch");
    expect(!scenario.writable, "uncertain permission change failure must retry saved restoration");
    expect(scenario.destroyed == 1, "unexposed JIT must be reclaimed after successful restoration");
}
void existing_foreign_hook_is_rejected_without_effects() {
    NativeHookManagerTest test;
    scenario.installed = true;
    unsigned long long id = 0;
    std::string error;
    expect(!test.install(&id, &error), "an existing foreign hook must reject native installation");
    expect(scenario.captures == 0 && scenario.compiled == 0 && scenario.applies == 0,
           "foreign hook rejection must precede compilation and permission changes");
    test.manager.shutdown();
    expect(scenario.installed && scenario.destroy_calls == 0,
           "rejecting a foreign hook must never remove it");
}
void foreign_hook_racing_prepare_is_not_claimed() {
    NativeHookManagerTest test;
    scenario.prepare_failure = true;
    scenario.foreign_hook_races_prepare = true;
    unsigned long long id = 0;
    std::string error;
    expect(!test.install(&id, &error),
           "a foreign hook installed during prepare must reject activation");
    expect(scenario.installed && scenario.observations_before_return == 0,
           "prepare rejection must not publish this manager's replacement");
    expect(!test.uninstall(test.retained_id(id), &error),
           "native.uninstall must not claim a rejected foreign hook");
    test.manager.shutdown();
    expect(scenario.installed && scenario.destroy_calls == 0,
           "native cleanup must leave the competing foreign hook untouched");
    expect(!scenario.writable && scenario.destroyed == 1,
           "rejected unpublished code must be reclaimed after protection restoration");
}
void original_is_ready_before_install_returns() {
    NativeHookManagerTest test;
    unsigned long long id = 0;
    std::string error;
    expect(test.install(&id, &error), error);
    expect(scenario.observations_before_return == 1,
           "backend must invoke preparation before returning");
    expect(!scenario.writable, "successful install must restore permissions");
    test.check_callback_while_control_lock_is_held(scenario.context, id);
    expect(NativeHookManager::original_for_id(id) == cold_target,
           "compatibility lookup must find published original");
    expect(test.uninstall(id, &error), error);
}
void prepare_failure_cleans_up_unpublished_module() {
    NativeHookManagerTest test;
    scenario.prepare_failure = true;
    unsigned long long id = 0;
    std::string error;
    expect(!test.install(&id, &error), "backend prepare failure must fail install");
    expect(!scenario.installed && !scenario.writable,
           "prepare failure must leave original entry/protection");
    expect(scenario.destroyed == 1, "unexposed JIT module must be reclaimed");
}
void activation_error_retains_exposed_code() {
    NativeHookManagerTest test;
    scenario.activated_failure = true;
    unsigned long long id = 0;
    std::string error;
    expect(!test.install(&id, &error), "activation error must be reported");
    expect(scenario.destroyed == 0,
           "code exposed to replacement cannot be unmapped on install failure");
    check_original(scenario.context);
    if (scenario.installed) {
        scenario.activated_failure = false;
        expect(test.uninstall(test.retained_id(id), &error), error);
    }
}
void backend_failure_stage_survives_cleanup() {
    for (const char *stage : {"near-allocation", "forward-patch", "original-relocation",
                              "entry-activation"}) {
        NativeHookManagerTest test;
        scenario.prepare_failure = std::strcmp(stage, "entry-activation") != 0;
        scenario.activated_failure = !scenario.prepare_failure;
        scenario.backend_failure_stage = stage;
        scenario.overwrite_stage_on_restore = true;
        unsigned long long id = 0;
        std::string error;
        expect(!test.install(&id, &error), "backend failure must fail installation");
        expect(error.find(std::string("stage=") + stage) != std::string::npos,
               "control error must preserve the backend failure stage before cleanup: " + error);
        expect(error.find("cleanup-overwrote-stage") == std::string::npos,
               "cleanup must not replace the install failure diagnosis");
        expect(!scenario.writable, "failed installation must restore saved permissions");
        if (scenario.installed)
            expect(test.uninstall(test.retained_id(id), &error), error);
    }
}
void rejected_mutation_does_not_borrow_outer_diagnosis() {
    NativeHookManagerTest test;
    scenario.mutation_rejected = true;
    scenario.overwrite_stage_on_restore = true;
    unsigned long long id = 0;
    std::string error;
    expect(!test.install(&id, &error), "nested mutation rejection must fail installation");
    expect(error.find("reentrant") != std::string::npos && error.find("status -3") != std::string::npos,
           "control error must explain the distinct mutation rejection: " + error);
    expect(scenario.stage_queries == 0 && error.find("outer-operation-stage") == std::string::npos &&
               error.find("cleanup-overwrote-stage") == std::string::npos,
           "a rejected request must not attribute the enclosing operation's diagnosis to itself");
    expect(!scenario.installed && !scenario.writable && scenario.destroyed == 1 &&
               scenario.observations_before_return == 0,
           "rejected mutation restores permissions and reclaims unexposed code");
}
void restoration_error_is_visible_and_retryable() {
    NativeHookManagerTest test;
    scenario.restore_failure = true;
    unsigned long long id = 0;
    std::string error;
    expect(!test.install(&id, &error), "permission restoration failure must be reported");
    id = test.retained_id(id);
    expect(error.find("restor") != std::string::npos, "error must explain restoration failure");
    expect(scenario.destroyed == 0, "restoration failure must retain exposed code");
    check_original(scenario.context);
    scenario.restore_failure = false;
    expect(test.uninstall(id, &error), error);
    expect(!scenario.installed && !scenario.writable,
           "retry must restore both entry and page permissions");
}
void failed_uninstall_can_be_retried() {
    NativeHookManagerTest test;
    unsigned long long id = 0;
    std::string error;
    expect(test.install(&id, &error), error);
    scenario.destroy_failure = true;
    expect(!test.uninstall(id, &error), "failed backend removal must be reported");
    expect(scenario.installed && scenario.destroyed == 0,
           "failed removal must retain active replacement");
    check_original(scenario.context);
    scenario.destroy_failure = false;
    expect(test.uninstall(id, &error), error);
}
void in_flight_replacement_survives_uninstall() {
    NativeHookManagerTest test;
    unsigned long long id = 0;
    std::string error;
    expect(test.install(&id, &error), error);
    const auto context = scenario.context;
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool resume = false;
    std::thread callback([&] {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        condition.notify_all();
        condition.wait(lock, [&] { return resume; });
        lock.unlock();
        check_original(context);
        NativeHookManager::hit_for_context(context);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return entered; });
    }
    expect(test.uninstall(id, &error), error);
    expect(scenario.destroyed == 0, "uninstall must not unmap code used by an in-flight callback");
    expect(NativeHookManager::original_for_id(id) == cold_target,
           "uninstall must preserve compatibility original lookup");
    expect(test.list().find("uninstalled") != std::string::npos,
           "runtime status must distinguish an uninstalled record");
    {
        std::lock_guard<std::mutex> lock(mutex);
        resume = true;
    }
    condition.notify_all();
    callback.join();
}
void shutdown_preserves_published_context() {
    unsigned long long context = 0;
    unsigned long long published_id = 0;
    {
        NativeHookManagerTest test;
        unsigned long long id = 0;
        std::string error;
        expect(test.install(&id, &error), error);
        context = scenario.context;
        published_id = id;
        test.manager.shutdown();
        expect(scenario.destroyed == 0, "shutdown must preserve published code and state");
    }
    check_original(context);
    NativeHookManager::hit_for_context(context);
    expect(NativeHookManager::original_for_id(published_id) == cold_target,
           "shutdown must preserve compatibility lookup for in-flight callbacks");
}
} // namespace

int main(int argc, char **argv) {
    expect(argc == 2 || argc == 3, "pass cold fixture shared library and optional lifetime mode");
    library = argv[1];
    void *handle = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    expect(handle != nullptr, "load cold exported-symbol fixture");
    cold_target = dlsym(handle, "dejavu_test_cold_target");
    expect(cold_target != nullptr, "resolve cold exported test function");
    if (argc == 3) {
        expect(std::strcmp(argv[2], "--address-pin") == 0, "unknown lifetime mode");
        isolated_address_module_lifetime(handle);
        return 0;
    }
    const struct {
        const char *name;
        void (*run)();
    } tests[] = {
        {"compile failure leaves XOM untouched", compile_failure_never_changes_permissions},
        {"missing entry leaves XOM untouched", missing_entry_never_changes_permissions},
        {"invalid mapping rejects before compilation", capture_failure_never_compiles_or_patches},
        {"permission apply failure restores saved protection",
         apply_failure_restores_permissions_before_returning},
        {"existing foreign hook is untouched", existing_foreign_hook_is_rejected_without_effects},
        {"foreign hook racing prepare is not claimed", foreign_hook_racing_prepare_is_not_claimed},
        {"original is published before activation returns",
         original_is_ready_before_install_returns},
        {"unpublished prepare failure reclaims code", prepare_failure_cleans_up_unpublished_module},
        {"activation failure retains exposed code", activation_error_retains_exposed_code},
        {"backend failure stage survives permission cleanup", backend_failure_stage_survives_cleanup},
        {"rejected mutation preserves the outer diagnosis", rejected_mutation_does_not_borrow_outer_diagnosis},
        {"restoration failure is retryable", restoration_error_is_visible_and_retryable},
        {"uninstall failure is retryable", failed_uninstall_can_be_retried},
        {"in-flight replacement survives uninstall", in_flight_replacement_survives_uninstall},
        {"shutdown preserves published context", shutdown_preserves_published_context},
    };
    address_registration_and_validation();
    std::cout << "OK: address registration, strict parsing, alignment and executable mapping validation\n";
    address_hex_file_and_shared_duplicate_guard();
    std::cout << "OK: hexadecimal address, device source and shared duplicate guard\n";
    caller_owned_address_targets();
    std::cout << "OK: namespace-invisible and anonymous targets install inline/from-file with explicit caller-owned lifetime\n";
    for (bool use_address : {false, true}) {
        address_mode = use_address;
        for (const auto &test : tests) {
            test.run();
            std::cout << "OK: " << (use_address ? "address: " : "symbol: ") << test.name << '\n';
        }
    }
    dlclose(handle);
    // Published fake modules mirror the manager's documented process-lifetime retention.
    for (auto *module : modules)
        delete module;
    return 0;
}
