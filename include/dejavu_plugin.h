#pragma once

/*
 * Dejavu native plugin ABI (v2, append-only).
 *
 * A plugin is an arm64 shared object exporting:
 *
 *   extern "C" __attribute__((visibility("default")))
 *   const DejavuPluginDescriptor *dejavu_plugin_entry(void);
 *
 * The agent dlopen()s the library, validates abi_version + struct_size, calls
 * initialize(), invokes on_control_ready() once the Lua control session exists
 * (if provided), and later shutdown(). Plugins never link against agent
 * private symbols; all services are passed through DejavuPluginHost.
 */

#include <jni.h>
#include <stddef.h>
#include <stdint.h>

#include <dejavu.h>

#define DEJAVU_PLUGIN_ABI_VERSION 2u
#define DEJAVU_PLUGIN_ENTRY_SYMBOL "dejavu_plugin_entry"

/* Descriptor flags. */
#define DEJAVU_PLUGIN_FLAG_UNLOAD_SAFE 1u

/* Optional capability bits reported through the descriptor (append-only ABI). */
#define DEJAVU_PLUGIN_CAP_MEM 1u
#define DEJAVU_PLUGIN_CAP_MODULE 2u
#define DEJAVU_PLUGIN_CAP_HOOK 4u
#define DEJAVU_PLUGIN_CAP_FS 8u
#define DEJAVU_PLUGIN_CAP_JAVA 16u
#define DEJAVU_PLUGIN_CAP_STREAM 32u

/* Host service interface versions. */
#define DEJAVU_MEM_INTERFACE_VERSION 1u
#define DEJAVU_MODULE_INTERFACE_VERSION 1u
#define DEJAVU_HOOK_INTERFACE_VERSION 1u
#define DEJAVU_FS_INTERFACE_VERSION 1u
#define DEJAVU_STREAM_INTERFACE_VERSION 1u

/* Stream transports for DejavuStreamInterface.open(). */
#define DEJAVU_STREAM_TRANSPORT_TCP 1u
#define DEJAVU_STREAM_TRANSPORT_UDP 2u
#define DEJAVU_STREAM_TRANSPORT_KCP 3u

typedef struct DejavuMemInterface {
    uint32_t version;
    uint32_t struct_size;
    /* Safe same-process read/write. Returns 0 on success, -1 on failure. */
    int (*read)(void *host_context, uintptr_t address, void *buffer, size_t size);
    int (*write)(void *host_context, uintptr_t address, const void *buffer, size_t size);
    /* Look up the mapping containing address; start/end/prot may be NULL. */
    int (*query)(void *host_context, uintptr_t address, uintptr_t *start, uintptr_t *end,
                 uint32_t *prot);
} DejavuMemInterface;

typedef struct DejavuModuleInterface {
    uint32_t version;
    uint32_t struct_size;
    /* Resolve a symbol in a loaded library, including non-dynamic symbols. */
    void *(*resolve)(void *host_context, const char *library, const char *symbol);
} DejavuModuleInterface;

typedef struct DejavuHookInterface {
    uint32_t version;
    uint32_t struct_size;
    /* Inline-hook target to replacement; original receives the trampoline. */
    int (*install)(void *host_context, void *target, void *replacement, void **original,
                   char *error, size_t error_size);
    /* Remove a hook previously installed through this interface. */
    int (*uninstall)(void *host_context, void *target, char *error, size_t error_size);
} DejavuHookInterface;

typedef struct DejavuFsInterface {
    uint32_t version;
    uint32_t struct_size;
    /* Read up to capacity bytes; *size_read receives the bytes copied. */
    int (*read_file)(void *host_context, const char *path, void *buffer, size_t capacity,
                     size_t *size_read);
    /* Create/truncate path and write data. */
    int (*write_file)(void *host_context, const char *path, const void *data, size_t size);
    int (*exists)(void *host_context, const char *path);
    int (*file_size)(void *host_context, const char *path, unsigned long long *size);
    int (*remove_file)(void *host_context, const char *path);
} DejavuFsInterface;

typedef struct DejavuStreamInterface {
    uint32_t version;
    uint32_t struct_size;
    /*
     * Connect a stream to host:port using transport (TCP/UDP/KCP), returning a
     * positive handle owned by the calling plugin, or 0 on failure.
     */
    int (*open)(void *host_context, uint32_t transport, const char *endpoint, char *error,
                size_t error_size);
    /*
     * Send one framed record. The framework prefixes a 32-bit network-order
     * length; the viewer delivers the payload as one record.
     */
    int (*write)(void *host_context, int handle, const void *data, size_t size);
    int (*close)(void *host_context, int handle);
} DejavuStreamInterface;

typedef struct DejavuPluginHost {
    uint32_t abi_version;
    uint32_t struct_size;
    void *runtime; /* opaque DejaVu runtime */
    JavaVM *java_vm;
    void *host_context;

    /* Register function_name inside a Lua namespace. */
    int (*register_function)(void *host_context, const char *namespace_name,
                             const char *function_name, dejavu_host_function function,
                             void *function_context, char *error, size_t error_size);

    /* Android log priority (ANDROID_LOG_*) + message; returns 0 on success. */
    int (*log_message)(void *host_context, int priority, const char *message);

    /* Current app ClassLoader global ref, or NULL before control init. */
    jobject (*get_app_class_loader)(void *host_context);

    /* Result/string allocation helpers owned by the agent allocator. */
    void *(*alloc)(size_t size);
    void (*free)(void *pointer);

    /*
     * Look up an optional host service interface by name and version
     * ("mem", "module", "hook", "fs", "stream"). Returns NULL if unavailable.
     */
    const void *(*get_interface)(void *host_context, const char *name, uint32_t version);
} DejavuPluginHost;

typedef struct DejavuPluginDescriptor {
    uint32_t abi_version;
    uint32_t struct_size;
    const char *name;
    const char *version;
    uint32_t flags; /* DEJAVU_PLUGIN_FLAG_UNLOAD_SAFE */

    int (*initialize)(const DejavuPluginHost *host, void **plugin_state, char *error,
                      size_t error_size);

    /* Called once the Lua control session is ready (may be NULL). */
    void (*on_control_ready)(void *plugin_state);

    void (*shutdown)(void *plugin_state);

    /*
     * Optional and appended after the original fields; read it only when
     * struct_size reaches offsetof(capabilities) + sizeof(uint32_t).
     */
    uint32_t capabilities;

    /*
     * Optional pre-unload hook (read via struct_size): remove owned native
     * hooks here so unload can verify quiescence before shutdown/dlclose.
     */
    void (*on_unload_prepare)(void *plugin_state);
} DejavuPluginDescriptor;

typedef const DejavuPluginDescriptor *(*DejaVuPluginEntry)(void);

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((visibility("default"))) const DejavuPluginDescriptor *dejavu_plugin_entry(void);

#ifdef __cplusplus
}
#endif
