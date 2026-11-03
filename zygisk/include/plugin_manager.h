#pragma once

#include <jni.h>

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "dejavu.h"
#include "dejavu_plugin.h"
#include "root_helper_client.h"
#include "stream_service.h"

struct DejavuRuntime;

class PluginManager {
  public:
    PluginManager();
    ~PluginManager();

    PluginManager(const PluginManager &) = delete;
    PluginManager &operator=(const PluginManager &) = delete;

    bool initialize(DejavuRuntime *runtime, JavaVM *vm, std::string *error);
    bool attach_session(dejavu_control_session *session, dejavu_engine *engine, JNIEnv *env,
                        jobject app_class_loader, std::string *error);
    bool register_lua(dejavu_control_session *session, std::string *error);
    void shutdown();

    bool load_plugin(const std::string &path, std::string *status, std::string *error);
    bool load_jit_plugin(const std::string &path, std::string *status, std::string *error);
    bool register_jit_function(const char *namespace_name, const char *function_name,
                               void *function, void *function_context, char *error,
                               size_t error_size);
    bool unload_plugin(const std::string &name, std::string *status, std::string *error);
    int install_owned_hook(void *target, void *replacement, void **original,
                            char *error, size_t error_size);
    bool uninstall_owned_hook(void *target, char *error, size_t error_size);
    size_t hook_count_for(size_t plugin_index) const;
    bool active_plugin_has_capability(uint32_t capability) const;
    static int host_stream_open(void *host_context, uint32_t transport, const char *endpoint,
                                char *error, size_t error_size);
    static int host_stream_write(void *host_context, int handle, const void *data, size_t size);
    static int host_stream_close(void *host_context, int handle);
    std::string status() const;
    jobject app_class_loader() const {
        return app_class_loader_;
    }

  private:
    struct JitCacheEntry {
        dejavu_module *module = nullptr;
        unsigned int refs = 0;
        unsigned long long stamp = 0;
    };
    struct HookRecord {
        void *target = nullptr;
        size_t plugin_index = 0;
        RootHelperProtectionSnapshot protection;
        bool installed = false;
        bool permissions_dirty = false;
    };
    struct Entry {
        size_t plugin_index = 0;
        std::string namespace_name;
        std::string function_name;
        dejavu_host_function function = nullptr;
        void *context = nullptr;
        bool valid = false;
        std::atomic<unsigned int> in_flight{0};
    };
    struct Plugin {
        void *handle = nullptr;
        const DejavuPluginDescriptor *descriptor = nullptr;
        void *state = nullptr;
        std::string path;
        std::string name;
        std::string version;
        uint32_t flags = 0;
        uint32_t capabilities = 0;
        bool loaded = false;
        bool initialized = false;
        bool cleanup_pending = false;
        bool control_ready = false;
        bool jit = false;
        dejavu_module *jit_module = nullptr;
        bool jit_cache_owned = false;
        std::string jit_cache_key;
        void (*jit_shutdown)() = nullptr;
        void (*jit_prepare)() = nullptr;
        uint64_t stream_owner_token = 0;
        std::string skill_text;
    };

    static int host_register_function(void *host_context, const char *namespace_name,
                                      const char *function_name, dejavu_host_function function,
                                      void *function_context, char *error, size_t error_size);
    static int host_log_message(void *host_context, int priority, const char *message);
    static jobject host_get_app_class_loader(void *host_context);
    static void *host_alloc(size_t size);
    static void host_free(void *pointer);
    static const void *host_get_interface(void *host_context, const char *name, uint32_t version);
    static int host_mem_read(void *host_context, uintptr_t address, void *buffer, size_t size);
    static int host_mem_write(void *host_context, uintptr_t address, const void *buffer,
                              size_t size);
    static int host_mem_query(void *host_context, uintptr_t address, uintptr_t *start,
                              uintptr_t *end, uint32_t *prot);
    static void *host_module_resolve(void *host_context, const char *library, const char *symbol);
    static int host_hook_install(void *host_context, void *target, void *replacement,
                                 void **original, char *error, size_t error_size);
    static int host_hook_uninstall(void *host_context, void *target, char *error,
                                   size_t error_size);
    static int host_fs_read(void *host_context, const char *path, void *buffer, size_t capacity,
                            size_t *size_read);
    static int host_fs_write(void *host_context, const char *path, const void *data, size_t size);
    static int host_fs_exists(void *host_context, const char *path);
    static int host_fs_size(void *host_context, const char *path, unsigned long long *size);
    static int host_fs_remove(void *host_context, const char *path);
    static int trampoline(dejavu_control_session *session, void *context,
                          const dejavu_value *arguments, size_t argument_count,
                          dejavu_value *result, char *error, size_t error_size);

    static int lua_load(dejavu_control_session *, void *, const dejavu_value *, size_t,
                        dejavu_value *, char *, size_t);
    static int lua_load_jit(dejavu_control_session *, void *, const dejavu_value *, size_t,
                            dejavu_value *, char *, size_t);
    static int lua_list(dejavu_control_session *, void *, const dejavu_value *, size_t,
                        dejavu_value *, char *, size_t);
    static int lua_unload(dejavu_control_session *, void *, const dejavu_value *, size_t,
                          dejavu_value *, char *, size_t);
    static int lua_call(dejavu_control_session *, void *, const dejavu_value *, size_t,
                        dejavu_value *, char *, size_t);
    static int lua_skill(dejavu_control_session *, void *, const dejavu_value *, size_t,
                         dejavu_value *, char *, size_t);
    static int lua_status(dejavu_control_session *, void *, const dejavu_value *, size_t,
                          dejavu_value *, char *, size_t);

    bool register_entry(Entry *entry, char *error, size_t error_size);
    bool load_plugin_locked(const std::string &path, std::string *status, std::string *error);
    bool load_jit_plugin_locked(const std::string &path, std::string *status, std::string *error);
    void autoload_from_property();
    void load_plugin_manifest();
    void refresh_allowed_paths();
    bool whitelisted_path(const std::string &path) const;
    std::string expected_hash_for(const std::string &path) const;
    bool verify_skill_text(const std::string &path, const std::string &text,
                           std::string *error) const;
    bool verify_file_hash(const std::string &path, std::string *error) const;
    dejavu_module *compile_jit_source(const std::string &hash, const std::string &source,
                                      std::string *error);
    void release_jit_module(const std::string &hash, bool owned);
    void discard_jit_module(const std::string &hash, dejavu_module *module, bool cache_owned);
    void destroy_jit_cache();
    bool name_exists(const std::string &name) const;
    void invalidate_entries(size_t plugin_index);
    void prepare_plugin_for_unload(size_t plugin_index);
    bool remove_plugin(size_t plugin_index, std::string *error);
    bool remove_hooks_for(size_t plugin_index, std::string *error);
    bool uninstall_hook_record(size_t record_index, std::string *error);
    static bool valid_identifier(const std::string &value);
    static bool valid_namespace(const std::string &value);
    static bool make_string(const std::string &value, dejavu_value *result);
    static bool lua_string_argument(const dejavu_value *arguments, size_t count, size_t index,
                                    std::string *value);

    DejavuRuntime *runtime_ = nullptr;
    JavaVM *vm_ = nullptr;
    dejavu_control_session *session_ = nullptr;
    dejavu_engine *engine_ = nullptr;
    jobject app_class_loader_ = nullptr;
    DejavuPluginHost host_ = {};
    mutable std::recursive_mutex mutex_;
    std::vector<std::unique_ptr<Plugin>> plugins_;
    std::deque<std::unique_ptr<Entry>> entries_;
    std::vector<Entry *> pending_entries_;
    std::set<std::string> allowed_paths_;
    std::map<std::string, std::string> allowed_hashes_;
    std::map<std::string, std::string> allowed_skill_hashes_;
    std::vector<HookRecord> hooks_;
    std::map<std::string, JitCacheEntry> jit_cache_;
    unsigned long long jit_cache_clock_ = 0;
    std::unique_ptr<StreamService> streams_;
    uint64_t next_stream_owner_ = 1;
    bool registered_lua_ = false;
};
