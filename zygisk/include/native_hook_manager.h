#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dejavu.h"
#include "dejavu_runtime.h"

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
 *          native.uninstall(id), native.list(), native.stats(id), native.clear()
 *
 * The C source must define exactly one replacement function named
 * `dejavu_native_target` with the target function's signature. Inside it:
 *   void *dejavu_native_original(void);  // trampoline to the original function
 *   void dejavu_native_hit(void);        // increment this hook's call counter
 *   void dejavu_native_log(const char *message);
 *
 * Hooking requires the symbol to be exported by an already-loaded module
 * (`dlopen(module, RTLD_NOLOAD)` + `dlsym`), and the target must live in an
 * executable mapping. Use `native.uninstall(id)` to restore the original code.
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
    static void log_message(const char *message);
    static long long call_argument(unsigned int index);
    static long long call_argument_int(unsigned int index);
    static double call_argument_double(unsigned int index);
    static int call_argument_boolean(unsigned int index);
    static const char *call_argument_string(unsigned int index, unsigned long *size);
    static int call_argument_type(unsigned int index);
    static void call_return(long long value);

  private:
    struct Record {
        unsigned long long id = 0;
        std::string module_name;
        std::string symbol_name;
        void *module_handle = nullptr;
        void *target = nullptr;
        dejavu_module *module = nullptr;
        std::atomic<void *> original{nullptr};
        std::atomic<unsigned long long> hits{0};
        std::atomic<bool> active{false};
    };

    bool install(const std::string &module_name, const std::string &symbol_name,
                 const std::string &source, unsigned long long *id, std::string *error);
    bool uninstall(unsigned long long id, std::string *error);
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
    static int lua_call_file(dejavu_control_session *, void *, const dejavu_value *, size_t,
                             dejavu_value *, char *, size_t);

    static NativeHookManager *instance_;

    DejavuRuntime *runtime_ = nullptr;
    dejavu_engine *engine_ = nullptr;
    mutable std::mutex mutex_;
    std::unordered_map<unsigned long long, std::unique_ptr<Record>> records_;
    std::atomic<unsigned long long> next_id_{1};
};
