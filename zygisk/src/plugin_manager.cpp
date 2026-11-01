#include "plugin_manager.h"

#include <android/log.h>
#include <dlfcn.h>
#include <sys/system_properties.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "dejavu_runtime.h"

namespace {

constexpr char kLogTag[] = "DejavuPlugin";
constexpr char kAutoLoadProperty[] = "debug.dejavu.plugins";
constexpr char kAllowedModulePrefix[] = "/data/adb/modules/dejavu_zygisk/";
constexpr char kAllowedTmpPrefix[] = "/data/local/tmp/";
constexpr char kAllowedUserPrefix[] = "/data/user/";
constexpr char kAllowedUserDePrefix[] = "/data/user_de/";
constexpr char kAllowedDataPrefix[] = "/data/data/";

thread_local size_t g_loading_plugin_index = static_cast<size_t>(-1);

void log_line(int priority, const char *message) {
    __android_log_print(priority, kLogTag, "%s", message != nullptr ? message : "");
}

} // namespace

PluginManager::PluginManager() = default;

PluginManager::~PluginManager() {
    shutdown();
    if (app_class_loader_ != nullptr) {
        JNIEnv *env = nullptr;
        if (vm_ != nullptr &&
            vm_->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_OK &&
            env != nullptr) {
            env->DeleteGlobalRef(app_class_loader_);
            app_class_loader_ = nullptr;
        }
    }
}

void PluginManager::shutdown() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (auto &plugin : plugins_) {
        if (!plugin->loaded) {
            continue;
        }
        if (plugin->descriptor != nullptr && plugin->descriptor->shutdown != nullptr) {
            plugin->descriptor->shutdown(plugin->state);
        }
        for (auto &entry : entries_) {
            if (entry->plugin_index < plugins_.size() &&
                plugins_[entry->plugin_index].get() == plugin.get()) {
                entry->valid = false;
            }
        }
        plugin->loaded = false;
        plugin->state = nullptr;
        if (plugin->handle != nullptr) {
            dlclose(plugin->handle);
            plugin->handle = nullptr;
        }
    }
    plugins_.clear();
}

bool PluginManager::initialize(DejavuRuntime *runtime, JavaVM *vm, std::string *error) {
    if (runtime == nullptr || vm == nullptr) {
        *error = "invalid plugin manager initialization argument";
        return false;
    }
    runtime_ = runtime;
    vm_ = vm;
    host_.abi_version = DEJAVU_PLUGIN_ABI_VERSION;
    host_.runtime = runtime;
    host_.java_vm = vm;
    host_.host_context = this;
    host_.register_function = &PluginManager::host_register_function;
    host_.log_message = &PluginManager::host_log_message;
    host_.get_app_class_loader = &PluginManager::host_get_app_class_loader;
    autoload_from_property();
    return true;
}

void PluginManager::autoload_from_property() {
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get(kAutoLoadProperty, value) <= 0) {
        return;
    }
    std::string list(value);
    std::string current;
    std::vector<std::string> paths;
    for (char character : list) {
        if (character == ':' || character == ';' || character == '\n' || character == ' ') {
            if (!current.empty()) {
                paths.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(character);
        }
    }
    if (!current.empty()) {
        paths.push_back(current);
    }
    for (const std::string &path : paths) {
        std::string status;
        std::string load_error;
        if (!load_plugin(path, &status, &load_error)) {
            log_line(ANDROID_LOG_ERROR,
                     ("plugin autoload failed for " + path + ": " + load_error).c_str());
        } else {
            log_line(ANDROID_LOG_INFO, ("plugin autoloaded: " + status).c_str());
        }
    }
}

bool PluginManager::allowed_path(const std::string &path) {
    return path.size() > 1 && path[0] == '/' &&
           (path.rfind(kAllowedModulePrefix, 0) == 0 || path.rfind(kAllowedTmpPrefix, 0) == 0 ||
            path.rfind(kAllowedUserPrefix, 0) == 0 || path.rfind(kAllowedUserDePrefix, 0) == 0 ||
            path.rfind(kAllowedDataPrefix, 0) == 0);
}

bool PluginManager::load_plugin(const std::string &path, std::string *status, std::string *error) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return load_plugin_locked(path, status, error);
}

bool PluginManager::load_plugin_locked(const std::string &path, std::string *status,
                                       std::string *error) {
    if (!allowed_path(path)) {
        *error = "plugin path must be an absolute path under the module or /data/local/tmp";
        return false;
    }
    for (const auto &plugin : plugins_) {
        if (plugin->path == path && plugin->loaded) {
            *status = plugin->name + " already loaded from " + path;
            return true;
        }
    }
    void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char *dl_error = dlerror();
        *error = dl_error != nullptr ? dl_error : "dlopen failed";
        return false;
    }
    auto entry = reinterpret_cast<DejavuPluginEntry>(dlsym(handle, DEJAVU_PLUGIN_ENTRY_SYMBOL));
    if (entry == nullptr) {
        dlclose(handle);
        *error = "plugin does not export " DEJAVU_PLUGIN_ENTRY_SYMBOL;
        return false;
    }
    const DejavuPluginDescriptor *descriptor = entry();
    if (descriptor == nullptr || descriptor->abi_version != DEJAVU_PLUGIN_ABI_VERSION ||
        descriptor->name == nullptr || descriptor->initialize == nullptr) {
        dlclose(handle);
        *error = "plugin descriptor is incompatible";
        return false;
    }
    auto plugin = std::make_unique<Plugin>();
    plugin->handle = handle;
    plugin->descriptor = descriptor;
    plugin->path = path;
    plugin->name = descriptor->name;
    plugin->version = descriptor->version != nullptr ? descriptor->version : "";
    const size_t plugin_index = plugins_.size();
    plugins_.push_back(std::move(plugin));
    char plugin_error[512] = {};
    g_loading_plugin_index = plugin_index;
    const int init_status = descriptor->initialize(&host_, &plugins_[plugin_index]->state,
                                                   plugin_error, sizeof(plugin_error));
    g_loading_plugin_index = static_cast<size_t>(-1);
    if (init_status != 0) {
        dlclose(handle);
        *error = plugin_error[0] != '\0' ? plugin_error : "plugin initialize failed";
        plugins_[plugin_index]->loaded = false;
        plugins_[plugin_index]->handle = nullptr;
        return false;
    }
    plugins_[plugin_index]->loaded = true;
    *status =
        plugins_[plugin_index]->name +
        (plugins_[plugin_index]->version.empty() ? "" : " " + plugins_[plugin_index]->version);
    log_line(ANDROID_LOG_INFO, ("plugin loaded: " + *status + " from " + path).c_str());
    return true;
}

bool PluginManager::unload_plugin(const std::string &name, std::string *status,
                                  std::string *error) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Plugin *target = nullptr;
    size_t index = 0;
    for (size_t position = 0; position < plugins_.size(); ++position) {
        if (plugins_[position]->name == name) {
            target = plugins_[position].get();
            index = position;
            break;
        }
    }
    if (target == nullptr || !target->loaded) {
        *error = "plugin not loaded: " + name;
        return false;
    }
    if (target->descriptor != nullptr && target->descriptor->shutdown != nullptr) {
        target->descriptor->shutdown(target->state);
    }
    for (auto &entry : entries_) {
        if (entry->plugin_index == index) {
            entry->valid = false;
        }
    }
    target->loaded = false;
    target->state = nullptr;
    if (target->handle != nullptr) {
        dlclose(target->handle);
        target->handle = nullptr;
    }
    *status = name + " unloaded";
    return true;
}

bool PluginManager::register_lua(dejavu_control_session *session, std::string *error) {
    if (runtime_ == nullptr || session == nullptr) {
        *error = "plugin manager is not initialized";
        return false;
    }
    struct Registration {
        const char *name;
        dejavu_host_function function;
    };
    const Registration registrations[] = {
        {"load", &PluginManager::lua_load},     {"list", &PluginManager::lua_list},
        {"status", &PluginManager::lua_status}, {"unload", &PluginManager::lua_unload},
        {"call", &PluginManager::lua_call},
    };
    char runtime_error[512];
    for (const Registration &registration : registrations) {
        const int status = runtime_->control_session_register(session, "plugin", registration.name,
                                                              registration.function, this,
                                                              runtime_error, sizeof(runtime_error));
        if (status != DEJAVU_OK) {
            *error = runtime_error;
            return false;
        }
    }
    registered_lua_ = true;
    return true;
}

bool PluginManager::attach_session(dejavu_control_session *session, JNIEnv *env,
                                   jobject app_class_loader, std::string *error) {
    if (session == nullptr) {
        *error = "invalid plugin control session";
        return false;
    }
    session_ = session;
    if (env != nullptr && app_class_loader != nullptr) {
        if (app_class_loader_ != nullptr) {
            env->DeleteGlobalRef(app_class_loader_);
        }
        app_class_loader_ = env->NewGlobalRef(app_class_loader);
    }
    if (!register_lua(session, error)) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (Entry *entry : pending_entries_) {
        entry->valid = true;
        char register_error[512];
        const int status = runtime_->control_session_register(
            session_, entry->namespace_name.c_str(), entry->function_name.c_str(),
            &PluginManager::trampoline, entry, register_error, sizeof(register_error));
        if (status != DEJAVU_OK) {
            entry->valid = false;
            *error = register_error;
            return false;
        }
    }
    pending_entries_.clear();
    return true;
}

int PluginManager::host_register_function(void *host_context, const char *namespace_name,
                                          const char *function_name, dejavu_host_function function,
                                          void *function_context, char *error, size_t error_size) {
    auto *manager = static_cast<PluginManager *>(host_context);
    if (manager == nullptr || namespace_name == nullptr || function_name == nullptr ||
        function == nullptr) {
        std::snprintf(error, error_size, "plugin register_function got invalid arguments");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::recursive_mutex> lock(manager->mutex_);
    auto entry = std::make_unique<Entry>();
    entry->namespace_name = namespace_name;
    entry->function_name = function_name;
    entry->function = function;
    entry->context = function_context;
    entry->valid = true;
    entry->plugin_index =
        g_loading_plugin_index != static_cast<size_t>(-1) ? g_loading_plugin_index : 0;
    Entry *pointer = entry.get();
    manager->entries_.push_back(std::move(entry));
    if (manager->session_ != nullptr && manager->registered_lua_) {
        const int status = manager->runtime_->control_session_register(
            manager->session_, pointer->namespace_name.c_str(), pointer->function_name.c_str(),
            &PluginManager::trampoline, pointer, error, error_size);
        if (status != DEJAVU_OK) {
            pointer->valid = false;
            return status;
        }
    } else {
        manager->pending_entries_.push_back(pointer);
    }
    return DEJAVU_OK;
}

int PluginManager::host_log_message(void *, int priority, const char *message) {
    log_line(priority, message);
    return 0;
}

jobject PluginManager::host_get_app_class_loader(void *host_context) {
    auto *manager = static_cast<PluginManager *>(host_context);
    return manager != nullptr ? manager->app_class_loader_ : nullptr;
}

int PluginManager::trampoline(dejavu_control_session *session, void *context,
                              const dejavu_value *arguments, size_t argument_count,
                              dejavu_value *result, char *error, size_t error_size) {
    auto *entry = static_cast<Entry *>(context);
    if (entry == nullptr || !entry->valid || entry->function == nullptr) {
        std::snprintf(error, error_size, "plugin function is no longer loaded");
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    return entry->function(session, entry->context, arguments, argument_count, result, error,
                           error_size);
}

bool PluginManager::make_string(const std::string &value, dejavu_value *result) {
    char *copy = static_cast<char *>(malloc(value.size() + 1));
    if (copy == nullptr) {
        return false;
    }
    memcpy(copy, value.data(), value.size());
    copy[value.size()] = '\0';
    result->type = DEJAVU_VALUE_STRING;
    result->owned = 1;
    result->string_value = copy;
    result->string_size = value.size();
    return true;
}

bool PluginManager::lua_string_argument(const dejavu_value *arguments, size_t count, size_t index,
                                        std::string *value) {
    if (index >= count || arguments[index].type != DEJAVU_VALUE_STRING ||
        arguments[index].string_value == nullptr) {
        return false;
    }
    *value = std::string(arguments[index].string_value, arguments[index].string_size);
    return true;
}

int PluginManager::lua_load(dejavu_control_session *, void *context, const dejavu_value *arguments,
                            size_t argument_count, dejavu_value *result, char *error,
                            size_t error_size) {
    auto *manager = static_cast<PluginManager *>(context);
    std::string path;
    if (manager == nullptr || argument_count != 1 ||
        !lua_string_argument(arguments, argument_count, 0, &path)) {
        std::snprintf(error, error_size, "plugin.load expects an absolute .so path");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::string status;
    std::string load_error;
    if (!manager->load_plugin(path, &status, &load_error)) {
        std::snprintf(error, error_size, "%s", load_error.c_str());
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    if (!make_string(status, result)) {
        std::snprintf(error, error_size, "unable to allocate plugin status");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    return DEJAVU_OK;
}

std::string PluginManager::status() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::ostringstream output;
    output << "plugins=" << plugins_.size() << " entries=" << entries_.size();
    for (const auto &plugin : plugins_) {
        output << " [" << plugin->name;
        if (!plugin->version.empty()) {
            output << " " << plugin->version;
        }
        output << " loaded=" << (plugin->loaded ? "yes" : "no") << " path=" << plugin->path << "]";
    }
    return output.str();
}

int PluginManager::lua_list(dejavu_control_session *, void *context, const dejavu_value *,
                            size_t argument_count, dejavu_value *result, char *error,
                            size_t error_size) {
    auto *manager = static_cast<PluginManager *>(context);
    if (manager == nullptr || argument_count != 0) {
        std::snprintf(error, error_size, "plugin.list takes no arguments");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    if (!make_string(manager->status(), result)) {
        std::snprintf(error, error_size, "unable to allocate plugin status");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    return DEJAVU_OK;
}

int PluginManager::lua_status(dejavu_control_session *, void *context, const dejavu_value *,
                              size_t argument_count, dejavu_value *result, char *error,
                              size_t error_size) {
    return lua_list(nullptr, context, nullptr, argument_count, result, error, error_size);
}

int PluginManager::lua_unload(dejavu_control_session *, void *context,
                              const dejavu_value *arguments, size_t argument_count,
                              dejavu_value *result, char *error, size_t error_size) {
    auto *manager = static_cast<PluginManager *>(context);
    std::string name;
    if (manager == nullptr || argument_count != 1 ||
        !lua_string_argument(arguments, argument_count, 0, &name)) {
        std::snprintf(error, error_size, "plugin.unload expects a plugin name");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::string status;
    std::string unload_error;
    if (!manager->unload_plugin(name, &status, &unload_error)) {
        std::snprintf(error, error_size, "%s", unload_error.c_str());
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    if (!make_string(status, result)) {
        std::snprintf(error, error_size, "unable to allocate plugin status");
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    return DEJAVU_OK;
}

int PluginManager::lua_call(dejavu_control_session *session, void *context,
                            const dejavu_value *arguments, size_t argument_count,
                            dejavu_value *result, char *error, size_t error_size) {
    auto *manager = static_cast<PluginManager *>(context);
    std::string plugin_name;
    std::string function_name;
    if (manager == nullptr || argument_count < 2 ||
        !lua_string_argument(arguments, argument_count, 0, &plugin_name) ||
        !lua_string_argument(arguments, argument_count, 1, &function_name)) {
        std::snprintf(error, error_size, "plugin.call expects plugin, function[, args...]");
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::recursive_mutex> lock(manager->mutex_);
    size_t plugin_index = static_cast<size_t>(-1);
    for (size_t position = 0; position < manager->plugins_.size(); ++position) {
        if (manager->plugins_[position]->name == plugin_name &&
            manager->plugins_[position]->loaded) {
            plugin_index = position;
            break;
        }
    }
    if (plugin_index == static_cast<size_t>(-1)) {
        std::snprintf(error, error_size, "plugin not loaded: %s", plugin_name.c_str());
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    std::string qualified = function_name;
    const size_t dot = qualified.find('.');
    std::string requested_namespace =
        dot == std::string::npos ? std::string() : qualified.substr(0, dot);
    std::string requested_function =
        dot == std::string::npos ? qualified : qualified.substr(dot + 1);
    for (auto &entry : manager->entries_) {
        if (entry->plugin_index != plugin_index || !entry->valid) {
            continue;
        }
        if (entry->function_name != requested_function) {
            continue;
        }
        if (!requested_namespace.empty() && entry->namespace_name != requested_namespace) {
            continue;
        }
        return entry->function(session, entry->context, arguments + 2, argument_count - 2, result,
                               error, error_size);
    }
    std::snprintf(error, error_size, "plugin function not found: %s", function_name.c_str());
    return DEJAVU_ERROR_LUA_EXECUTE;
}
