#pragma once

/*
 * Dejavu native plugin ABI.
 *
 * A plugin is a shared object exporting:
 *
 *   extern "C" __attribute__((visibility("default")))
 *   const DejavuPluginDescriptor *dejavu_plugin_entry(void);
 *
 * The agent dlopen()s the library, validates the ABI version, calls
 * initialize() and later shutdown().  Plugins receive a host table with stable
 * function pointers and can register Lua functions; they never link against
 * the agent's private symbols.
 */

#include <jni.h>
#include <stddef.h>
#include <stdint.h>

#include <dejavu.h>

#define DEJAVU_PLUGIN_ABI_VERSION 1u

typedef struct DejavuPluginDescriptor DejavuPluginDescriptor;
typedef const DejavuPluginDescriptor *(*DejavuPluginEntry)(void);
#define DEJAVU_PLUGIN_ENTRY_SYMBOL "dejavu_plugin_entry"

typedef struct DejavuPluginHost {
    uint32_t abi_version;
    void *runtime; /* opaque DejaVu runtime */
    JavaVM *java_vm;
    void *host_context;

    /* Register function_name inside a Lua namespace ("unpack", ...). */
    int (*register_function)(void *host_context, const char *namespace_name,
                             const char *function_name, dejavu_host_function function,
                             void *function_context, char *error, size_t error_size);

    /* Android log priority (ANDROID_LOG_*) + message; returns 0 on success. */
    int (*log_message)(void *host_context, int priority, const char *message);

    /* Current app ClassLoader global ref, or NULL before control init. */
    jobject (*get_app_class_loader)(void *host_context);
} DejavuPluginHost;

struct DejavuPluginDescriptor {
    uint32_t abi_version;
    const char *name;
    const char *version;
    int (*initialize)(const DejavuPluginHost *host, void **plugin_state, char *error,
                      size_t error_size);
    void (*shutdown)(void *plugin_state);
};

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((visibility("default"))) const DejavuPluginDescriptor *dejavu_plugin_entry(void);

#ifdef __cplusplus
}
#endif
