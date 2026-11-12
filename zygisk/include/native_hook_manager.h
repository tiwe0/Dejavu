#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dejavu.h"
#include "dejavu_runtime.h"
#include "root_helper_client.h"

/* Typed Lua -> C argument protocol used by native.call(). */
struct NativeCallArgument {
    enum Type {
        kNull = 0,
        kBoolean = 1,
        kInteger = 2,
        kNumber = 3,
        kString = 4,
    };

    Type type = kNull;
    bool boolean_value = false;
    long long integer_value = 0;
    double number_value = 0.0;
    std::string string_value;
};

/*
 * Native function hooks (ABI 1).
 *
 * Lua API: native.install(module, symbol, c_source) -> id
 *          native.install_address(address, c_source) -> id
 *          native.uninstall(id), native.list(), native.stats(id), native.clear()
 *
 * The C source must define exactly one replacement function named
 * `dejavu_native_target` with the target function's signature. Inside it:
 *   void *dejavu_native_original(void);  // trampoline to the original function
 *   void dejavu_native_hit(void);        // increment this hook's call counter
 *   void dejavu_native_log(const char *message);
 *
 * Targets may be resolved from a loaded module's dynamic/ELF symbol table or
 * supplied as an absolute runtime address. They must live in executable mappings.
 * Use `native.uninstall(id)` to restore the original code and page protections.
 * Absolute-address callers must keep the same target mapping valid during
 * installation, while active, and through all in-flight callbacks or later uses
 * of a saved original. Image references are retained when namespace visibility
 * permits; otherwise the target is explicitly caller-owned. Uninstall does not
 * wait for these users.
 */
class NativeHookManager {
  public:
    NativeHookManager();
    ~NativeHookManager();

    NativeHookManager(const NativeHookManager &) = delete;
    NativeHookManager &operator=(const NativeHookManager &) = delete;

    bool initialize(DejavuRuntime *runtime, dejavu_engine *engine, std::string *error);
    bool register_lua(dejavu_control_session *session, std::string *error);
    void shutdown();

    /* Actively invoke an exported function through a TinyCC-generated wrapper.
     * The C source defines `void dejavu_native_call(void)` and uses
     * DEJAVU_NATIVE_CALL_ADDRESS / dejavu_native_call_argument(i) /
     * dejavu_native_call_return(value). */
    bool call(const std::string &module_name, const std::string &symbol_name,
              const std::string &source, const std::vector<NativeCallArgument> &arguments,
              long long *result, std::string *error);

    /* C helper entry points, also registered as TinyCC symbols. */
    static void *original_for_id(unsigned long long id);
    static void hit_for_id(unsigned long long id);
    static void *original_for_context(unsigned long long context);
    static void hit_for_context(unsigned long long context);
    static void log_message(const char *message);
    static long long call_argument(unsigned int index);
    static long long call_argument_int(unsigned int index);
    static double call_argument_double(unsigned int index);
    static int call_argument_boolean(unsigned int index);
    static const char *call_argument_string(unsigned int index, unsigned long *size);
    static int call_argument_type(unsigned int index);
    static void call_return(long long value);

  private:
    friend struct NativeHookManagerTest;
    struct Record {
        unsigned long long id = 0;
        std::string module_name;
        std::string symbol_name;
        void *module_handle = nullptr;
        bool caller_owned_target = false;
        void *target = nullptr;
        dejavu_module *module = nullptr;
        std::atomic<void *> original{nullptr};
        std::atomic<unsigned long long> hits{0};
        std::atomic<bool> active{false};
        std::atomic<bool> exposed{false};
        std::atomic<bool> restore_pending{false};
        RootHelperProtectionSnapshot protection;
        Record *callback_next = nullptr;
    };

    bool install(const std::string &module_name, const std::string &symbol_name,
                 const std::string &source, unsigned long long *id, std::string *error);
    bool install_address(uintptr_t address, const std::string &source,
                         unsigned long long *id, std::string *error);
    // Caller holds operations_mutex_ and transfers the resolved library reference.
    bool install_resolved(std::unique_ptr<Record> owned, const std::string &source,
                          unsigned long long *id, std::string *error);
    bool uninstall(unsigned long long id, std::string *error);
    bool uninstall_locked(unsigned long long id, std::string *error);
    void release_unexposed(Record *record);
    static void publish_original(void *original, size_t patch_size, void *context);
    bool clear(unsigned long long *removed, std::string *error);
    std::string list() const;
    bool stats(unsigned long long id, unsigned long long *hits, std::string *error) const;

    static int lua_install(dejavu_control_session *, void *, const dejavu_value *, size_t,
                           dejavu_value *, char *, size_t);
    static int lua_uninstall(dejavu_control_session *, void *, const dejavu_value *, size_t,
                             dejavu_value *, char *, size_t);
    static int lua_list(dejavu_control_session *, void *, const dejavu_value *, size_t,
                        dejavu_value *, char *, size_t);
    static int lua_stats(dejavu_control_session *, void *, const dejavu_value *, size_t,
                         dejavu_value *, char *, size_t);
    static int lua_clear(dejavu_control_session *, void *, const dejavu_value *, size_t,
                         dejavu_value *, char *, size_t);
    static int lua_call(dejavu_control_session *, void *, const dejavu_value *, size_t,
                        dejavu_value *, char *, size_t);
    static int lua_install_file(dejavu_control_session *, void *, const dejavu_value *, size_t,
                                dejavu_value *, char *, size_t);
    static int lua_install_address(dejavu_control_session *, void *, const dejavu_value *, size_t,
                                   dejavu_value *, char *, size_t);
    static int lua_install_address_file(dejavu_control_session *, void *, const dejavu_value *, size_t,
                                        dejavu_value *, char *, size_t);
    static int lua_install_address_impl(bool from_file, void *, const dejavu_value *, size_t,
                                       dejavu_value *, char *, size_t);
    static int lua_call_file(dejavu_control_session *, void *, const dejavu_value *, size_t,
                             dejavu_value *, char *, size_t);

    static std::atomic<Record *> callback_records_;
    static std::atomic<unsigned long long> next_id_;

    DejavuRuntime *runtime_ = nullptr;
    dejavu_engine *engine_ = nullptr;
    mutable std::mutex mutex_;
    std::mutex operations_mutex_;
    std::unordered_map<unsigned long long, std::unique_ptr<Record>> records_;
};
