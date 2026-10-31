#pragma once

#include <jni.h>

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "dejavu.h"
#include "dejavu_runtime.h"

/*
 * Hook-based unpacking helpers.
 *
 * unpack.enable(dir) installs a dlopen/android_dlopen_ext hook. The hook calls
 * back into the agent, which installs ART hooks on the available
 * DexFile::OpenCommon / ClassLinker::RegisterDexFile symbols and dumps the
 * referenced DEX images into `dir`. ART is usually already loaded, so the
 * initial enable() also installs the ART hooks immediately; the dlopen hook
 * additionally covers late library loads and logs the event.
 *
 * Lua API:
 *   unpack.enable(dir)              -> status string
 *   unpack.disable()                -> true
 *   unpack.status()                 -> status string
 *   unpack.install_art_hooks()      -> status string
 *   unpack.dump_memory(addr,size[,label]) -> file path
 */
class UnpackManager {
  public:
    UnpackManager();
    ~UnpackManager();

    UnpackManager(const UnpackManager &) = delete;
    UnpackManager &operator=(const UnpackManager &) = delete;

    bool initialize(DejavuRuntime *runtime, JavaVM *vm, std::string *error);
    bool register_lua(dejavu_control_session *session, std::string *error);
    void shutdown();
    void enable_from_system_property();
    bool hook_invoke(std::string *error);
    bool active_invoke(const std::string &filter, std::string *stats, std::string *error);
    void on_active_invoke();
    void set_app_class_loader(JNIEnv *env, jobject loader);

    static UnpackManager *instance();
    void on_library_loaded(const char *filename);
    void on_register_dex_file(void *dex_file);
    void try_dump_pointer(unsigned long long value, const char *label);

  private:
    static int lua_enable(dejavu_control_session *, void *, const dejavu_value *, size_t,
                          dejavu_value *, char *, size_t);
    static int lua_disable(dejavu_control_session *, void *, const dejavu_value *, size_t,
                           dejavu_value *, char *, size_t);
    static int lua_status(dejavu_control_session *, void *, const dejavu_value *, size_t,
                          dejavu_value *, char *, size_t);
    static int lua_install_art_hooks(dejavu_control_session *, void *, const dejavu_value *, size_t,
                                     dejavu_value *, char *, size_t);
    static int lua_dump_memory(dejavu_control_session *, void *, const dejavu_value *, size_t,
                               dejavu_value *, char *, size_t);
    static int lua_include_file_backed(dejavu_control_session *, void *, const dejavu_value *,
                                       size_t, dejavu_value *, char *, size_t);
    static int lua_hook_invoke(dejavu_control_session *, void *, const dejavu_value *, size_t,
                               dejavu_value *, char *, size_t);
    static int lua_active_invoke(dejavu_control_session *, void *, const dejavu_value *, size_t,
                                 dejavu_value *, char *, size_t);

    bool enable(const std::string &directory, std::string *error);
    bool install_dlopen_hooks(std::string *error);
    bool install_art_hooks(std::string *error);
    bool dump_dex(unsigned long long address, size_t size, const char *label);
    bool dump_discovered_revision(std::string *error);
    JNIEnv *current_env();
    struct Image {
        uintptr_t begin = 0;
        size_t size = 0;
    };
    bool dump_bytes(unsigned long long address, size_t size, const char *label, bool require_dex,
                    std::string *path, std::string *error);
    std::string status() const;

    static UnpackManager *instance_;

    DejavuRuntime *runtime_ = nullptr;
    JavaVM *vm_ = nullptr;
    mutable std::mutex mutex_;
    std::vector<Image> images_;
    bool invoke_hooked_ = false;
    unsigned long invoke_address_ = 0;
    std::atomic<unsigned long long> active_invoke_hits_{0};
    jobject app_class_loader_ = nullptr;
    std::string directory_;
    std::atomic<bool> enabled_{false};
    std::atomic<bool> include_file_backed_{false};
    std::atomic<unsigned long long> sequence_{0};
    std::set<std::string> dumped_keys_;
    unsigned long long dlopen_events_ = 0;
    bool dlopen_hooked_ = false;
    bool art_hooked_ = false;
    unsigned long long register_dexfile_address_ = 0;
    unsigned long long register_dexfile_locked_address_ = 0;
    unsigned long long opencommon_address_ = 0;
    unsigned long long dumped_count_ = 0;
    unsigned long long skipped_file_backed_ = 0;
    unsigned long long revision_dumps_ = 0;
};
