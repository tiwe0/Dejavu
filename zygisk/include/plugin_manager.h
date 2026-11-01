#pragma once

#include <jni.h>

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "dejavu.h"
#include "dejavu_plugin.h"

struct DejavuRuntime;

class PluginManager {
  public:
    PluginManager();
    ~PluginManager();

    PluginManager(const PluginManager &) = delete;
    PluginManager &operator=(const PluginManager &) = delete;

    bool initialize(DejavuRuntime *runtime, JavaVM *vm, std::string *error);
    bool attach_session(dejavu_control_session *session, JNIEnv *env, jobject app_class_loader,
                        std::string *error);
    bool register_lua(dejavu_control_session *session, std::string *error);
    void shutdown();

    bool load_plugin(const std::string &path, std::string *status, std::string *error);
    bool unload_plugin(const std::string &name, std::string *status, std::string *error);
    std::string status() const;

  private:
    struct Entry {
        size_t plugin_index = 0;
        std::string namespace_name;
        std::string function_name;
        dejavu_host_function function = nullptr;
        void *context = nullptr;
        bool valid = false;
    };
    struct Plugin {
        void *handle = nullptr;
        const DejavuPluginDescriptor *descriptor = nullptr;
        void *state = nullptr;
        std::string path;
        std::string name;
        std::string version;
        bool loaded = false;
    };

    static int host_register_function(void *host_context, const char *namespace_name,
                                      const char *function_name, dejavu_host_function function,
                                      void *function_context, char *error, size_t error_size);
    static int host_log_message(void *host_context, int priority, const char *message);
    static jobject host_get_app_class_loader(void *host_context);
    static int trampoline(dejavu_control_session *session, void *context,
                          const dejavu_value *arguments, size_t argument_count,
                          dejavu_value *result, char *error, size_t error_size);

    static int lua_load(dejavu_control_session *, void *, const dejavu_value *, size_t,
                        dejavu_value *, char *, size_t);
    static int lua_list(dejavu_control_session *, void *, const dejavu_value *, size_t,
                        dejavu_value *, char *, size_t);
    static int lua_unload(dejavu_control_session *, void *, const dejavu_value *, size_t,
                          dejavu_value *, char *, size_t);
    static int lua_call(dejavu_control_session *, void *, const dejavu_value *, size_t,
                        dejavu_value *, char *, size_t);
    static int lua_status(dejavu_control_session *, void *, const dejavu_value *, size_t,
                          dejavu_value *, char *, size_t);

    bool register_entry(Entry *entry, char *error, size_t error_size);
    bool load_plugin_locked(const std::string &path, std::string *status, std::string *error);
    void autoload_from_property();
    static bool allowed_path(const std::string &path);
    static bool make_string(const std::string &value, dejavu_value *result);
    static bool lua_string_argument(const dejavu_value *arguments, size_t count, size_t index,
                                    std::string *value);

    DejavuRuntime *runtime_ = nullptr;
    JavaVM *vm_ = nullptr;
    dejavu_control_session *session_ = nullptr;
    jobject app_class_loader_ = nullptr;
    DejavuPluginHost host_ = {};
    mutable std::recursive_mutex mutex_;
    std::vector<std::unique_ptr<Plugin>> plugins_;
    std::deque<std::unique_ptr<Entry>> entries_;
    std::vector<Entry *> pending_entries_;
    bool registered_lua_ = false;
};
