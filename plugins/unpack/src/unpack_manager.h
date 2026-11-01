#pragma once

#include <jni.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "dejavu.h"
#include "dejavu_plugin.h"

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

    bool initialize(const DejavuPluginHost *host, std::string *error);
    bool register_plugin_functions(const DejavuPluginHost *host, std::string *error);
    void shutdown();
    void enable_from_system_property();
    bool hook_invoke(std::string *error);
    bool install_hook(void *target, void *replacement, void **original, std::string *error);
    bool active_invoke(const std::string &filter, std::string *stats, std::string *error);
    bool start_active_invoke(const std::string &class_names, unsigned long long delay_ms,
                             std::string *status, std::string *error);
    bool wait_active_invoke(unsigned long long timeout_ms, std::string *status, std::string *error);
    void stop_active_invoke();
    void on_active_invoke();
    jobject current_app_class_loader();
    bool flush(unsigned long long timeout_ms, std::string *error);

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
    static int lua_flush(dejavu_control_session *, void *, const dejavu_value *, size_t,
                         dejavu_value *, char *, size_t);
    static int lua_active_invoke_async(dejavu_control_session *, void *, const dejavu_value *,
                                       size_t, dejavu_value *, char *, size_t);
    static int lua_active_invoke_wait(dejavu_control_session *, void *, const dejavu_value *,
                                      size_t, dejavu_value *, char *, size_t);
    static int lua_active_invoke_status(dejavu_control_session *, void *, const dejavu_value *,
                                        size_t, dejavu_value *, char *, size_t);
    static int lua_sleep_ms(dejavu_control_session *, void *, const dejavu_value *, size_t,
                            dejavu_value *, char *, size_t);
    static int lua_dump_revision(dejavu_control_session *, void *, const dejavu_value *, size_t,
                                 dejavu_value *, char *, size_t);

    bool enable(const std::string &directory, std::string *error);
    bool install_dlopen_hooks(std::string *error);
    bool install_art_hooks(std::string *error);
    bool dump_dex(unsigned long long address, size_t size, const char *label);
    bool enqueue_dex(unsigned long long address, size_t size, const char *label);
    bool dump_discovered_revision(std::string *error);
    void active_invoke_worker();
    void start_writer();
    void stop_writer();
    void writer_loop();
    JNIEnv *current_env();
    struct Image {
        uintptr_t begin = 0;
        size_t size = 0;
    };
    struct PendingDump {
        std::string label;
        std::string extension;
        std::vector<unsigned char> data;
    };
    bool dump_bytes(unsigned long long address, size_t size, const char *label, bool require_dex,
                    std::string *path, std::string *error);
    std::string status() const;

    static UnpackManager *instance_;

    const DejavuPluginHost *host_ = nullptr;
    const DejavuHookInterface *hook_interface_ = nullptr;
    JavaVM *vm_ = nullptr;
    mutable std::mutex mutex_;
    std::vector<Image> images_;
    bool invoke_hooked_ = false;
    unsigned long invoke_address_ = 0;
    std::atomic<unsigned long long> active_invoke_hits_{0};
    std::thread writer_;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<PendingDump> queue_;
    bool writer_stop_ = false;
    bool writer_writing_ = false;
    size_t pending_bytes_ = 0;
    size_t max_pending_bytes_ = 128 * 1024 * 1024;
    unsigned long long dropped_budget_ = 0;
    unsigned long long written_files_ = 0;
    std::thread active_thread_;
    mutable std::mutex active_mutex_;
    std::condition_variable active_cv_;
    std::atomic<bool> active_running_{false};
    std::atomic<bool> active_stop_{false};
    std::vector<std::string> active_names_;
    std::string active_error_;
    unsigned long long active_delay_ms_ = 40;
    unsigned long long active_total_ = 0;
    unsigned long long active_done_ = 0;
    std::atomic<unsigned long long> active_errors_{0};
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
