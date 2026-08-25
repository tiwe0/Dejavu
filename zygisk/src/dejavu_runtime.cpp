#include "dejavu_runtime.h"

#include <android/dlext.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace {

constexpr char kLibraryPath[] = "lib/arm64-v8a/libdejavu.so";

void set_error(char *error, size_t error_size, const char *message) {
    if (error == nullptr || error_size == 0) {
        return;
    }
    snprintf(error, error_size, "%s", message != nullptr ? message : "unknown error");
}

template <typename Function>
bool resolve(void *handle, const char *name, Function *function) {
    dlerror();
    void *symbol = dlsym(handle, name);
    if (symbol == nullptr || dlerror() != nullptr) {
        return false;
    }
    static_assert(sizeof(*function) == sizeof(symbol));
    memcpy(function, &symbol, sizeof(symbol));
    return true;
}

}  // namespace

bool dejavu_runtime_load(
    int module_dir_fd,
    DejavuRuntime *runtime,
    char *error,
    size_t error_size) {
    android_dlextinfo info = {};
    void *handle;
    int library_fd;

    set_error(error, error_size, "");
    if (module_dir_fd < 0 || runtime == nullptr) {
        set_error(error, error_size, "invalid Dejavu loader argument");
        return false;
    }
    memset(runtime, 0, sizeof(*runtime));

    library_fd = openat(module_dir_fd, kLibraryPath, O_RDONLY | O_CLOEXEC);
    if (library_fd < 0) {
        set_error(error, error_size, strerror(errno));
        return false;
    }

    info.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
    info.library_fd = library_fd;
    handle = android_dlopen_ext("libdejavu.so", RTLD_NOW | RTLD_LOCAL, &info);
    close(library_fd);
    if (handle == nullptr) {
        set_error(error, error_size, dlerror());
        return false;
    }

    runtime->handle = handle;
#define RESOLVE_DEJAVU(member, symbol) \
    do { \
        if (!resolve(handle, symbol, &runtime->member)) { \
            char message[128]; \
            snprintf(message, sizeof(message), "missing Dejavu ABI symbol: %s", symbol); \
            set_error(error, error_size, message); \
            dejavu_runtime_unload(runtime); \
            return false; \
        } \
    } while (false)
    RESOLVE_DEJAVU(version, "dejavu_version");
    RESOLVE_DEJAVU(lua_version, "dejavu_lua_version");
    RESOLVE_DEJAVU(tcc_version, "dejavu_tcc_version");
    RESOLVE_DEJAVU(engine_create, "dejavu_engine_create");
    RESOLVE_DEJAVU(engine_destroy, "dejavu_engine_destroy");
    RESOLVE_DEJAVU(engine_add_symbol, "dejavu_engine_add_symbol");
    RESOLVE_DEJAVU(engine_compile, "dejavu_engine_compile");
    RESOLVE_DEJAVU(module_symbol, "dejavu_module_symbol");
    RESOLVE_DEJAVU(module_call, "dejavu_module_call");
    RESOLVE_DEJAVU(module_destroy, "dejavu_module_destroy");
    RESOLVE_DEJAVU(control_session_create, "dejavu_control_session_create");
    RESOLVE_DEJAVU(control_session_destroy, "dejavu_control_session_destroy");
    RESOLVE_DEJAVU(control_session_register, "dejavu_control_session_register");
    RESOLVE_DEJAVU(control_session_eval, "dejavu_control_session_eval");
    RESOLVE_DEJAVU(control_session_invoke, "dejavu_control_session_invoke");
    RESOLVE_DEJAVU(
        control_session_release_function,
        "dejavu_control_session_release_function");
    RESOLVE_DEJAVU(value_release, "dejavu_value_release");
    RESOLVE_DEJAVU(control_run, "dejavu_control_run");
    RESOLVE_DEJAVU(smoke, "dejavu_smoke");
#undef RESOLVE_DEJAVU
    return true;
}

void dejavu_runtime_unload(DejavuRuntime *runtime) {
    if (runtime == nullptr) {
        return;
    }
    if (runtime->handle != nullptr) {
        dlclose(runtime->handle);
    }
    memset(runtime, 0, sizeof(*runtime));
}
