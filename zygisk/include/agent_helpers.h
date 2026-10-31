#pragma once

#include <map>
#include <mutex>
#include <string>

#include "dejavu.h"
#include "dejavu_runtime.h"

/*
 * Agent helper APIs exposed to the Lua control session:
 *
 *   fs.read(path)                -> string
 *   fs.write(path, data[, app])  -> integer bytes written
 *   fs.exists(path)              -> boolean
 *   fs.remove(path)              -> true
 *   fs.list(path)                -> newline-separated entries
 *   fs.stat(path)                -> "size=... mode=... mtime=..." string
 *
 *   module.load(path[, scope])   -> integer id (dlopen RTLD_NOW)
 *   module.unload(id)            -> true
 *   module.loaded()              -> newline-separated "id<TAB>path" lines
 *   module.symbols(target[, filter]) -> newline-separated symbol table
 *   module.resolve(target, symbol)   -> integer address or nil
 *
 * `target` is a module id from module.load(), or a library name/path that is
 * already loaded. File helpers run with the target process' UID and are not a
 * sandbox: only pass paths the target process is allowed to access.
 */
class AgentHelpers {
  public:
    AgentHelpers();
    ~AgentHelpers();

    AgentHelpers(const AgentHelpers &) = delete;
    AgentHelpers &operator=(const AgentHelpers &) = delete;

    bool initialize(DejavuRuntime *runtime, std::string *error);
    bool register_lua(dejavu_control_session *session, std::string *error);
    void shutdown();

  private:
    struct Library {
        void *handle = nullptr;
        std::string path;
    };

    static int lua_fs_read(dejavu_control_session *, void *, const dejavu_value *, size_t,
                           dejavu_value *, char *, size_t);
    static int lua_fs_write(dejavu_control_session *, void *, const dejavu_value *, size_t,
                            dejavu_value *, char *, size_t);
    static int lua_fs_exists(dejavu_control_session *, void *, const dejavu_value *, size_t,
                             dejavu_value *, char *, size_t);
    static int lua_fs_remove(dejavu_control_session *, void *, const dejavu_value *, size_t,
                             dejavu_value *, char *, size_t);
    static int lua_fs_list(dejavu_control_session *, void *, const dejavu_value *, size_t,
                           dejavu_value *, char *, size_t);
    static int lua_fs_stat(dejavu_control_session *, void *, const dejavu_value *, size_t,
                           dejavu_value *, char *, size_t);

    static int lua_module_load(dejavu_control_session *, void *, const dejavu_value *, size_t,
                               dejavu_value *, char *, size_t);
    static int lua_module_unload(dejavu_control_session *, void *, const dejavu_value *, size_t,
                                 dejavu_value *, char *, size_t);
    static int lua_module_loaded(dejavu_control_session *, void *, const dejavu_value *, size_t,
                                 dejavu_value *, char *, size_t);
    static int lua_module_symbols(dejavu_control_session *, void *, const dejavu_value *, size_t,
                                  dejavu_value *, char *, size_t);
    static int lua_module_resolve(dejavu_control_session *, void *, const dejavu_value *, size_t,
                                  dejavu_value *, char *, size_t);

    bool library_path(long long id, std::string *path, std::string *error) const;

    DejavuRuntime *runtime_ = nullptr;
    mutable std::mutex mutex_;
    std::map<long long, Library> libraries_;
    long long next_library_id_ = 1;
};
