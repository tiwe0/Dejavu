#pragma once

/*
 * TinyCC JIT plugin ABI prelude.
 *
 * The agent prepends this header text to a JIT source before compiling it with
 * TinyCC. It is intentionally self-contained (no libc/include dependencies) and
 * append-only: new host functions/helpers may be added, existing declarations
 * must not change.
 *
 * The same header is exposed for documentation and editor completion. Runtime
 * variants are generated from this file at build time.
 */

#ifndef NULL
#define NULL ((void *)0)
#endif

#define DEJAVU_JIT_OK 0
#define DEJAVU_JIT_INVALID_ARGUMENT 1
#define DEJAVU_JIT_ERROR 2

typedef struct DejavuJitValue {
    int type;
    int owned;
    int boolean_value;
    long long integer_value;
    double number_value;
    const char *string_value;
    unsigned long string_size;
    void *handle_value;
    unsigned long long function_value;
    void *host_function;
    void *host_context;
} DejavuJitValue;

typedef int (*DejavuJitFunction)(void *session, void *context, const DejavuJitValue *arguments,
                                 unsigned long argument_count, DejavuJitValue *result, char *error,
                                 unsigned long error_size);

extern int dejavu_jit_register(const char *namespace_name, const char *function_name,
                               DejavuJitFunction function, void *context, char *error,
                               unsigned long error_size);
extern void *dejavu_jit_alloc(unsigned long size);
extern void dejavu_jit_free(void *pointer);
extern int dejavu_jit_log(int priority, const char *message);
extern void *dejavu_jit_get_app_class_loader(void);
extern int dejavu_jit_mem_read(unsigned long long address, void *buffer, unsigned long size);
extern int dejavu_jit_mem_write(unsigned long long address, const void *buffer, unsigned long size);
extern int dejavu_jit_mem_query(unsigned long long address, unsigned long long *start,
                                unsigned long long *end, unsigned int *prot);
extern void *dejavu_jit_module_resolve(const char *library, const char *symbol);
extern int dejavu_jit_hook_install(void *target, void *replacement, void **original, char *error,
                                   unsigned long error_size);
extern int dejavu_jit_hook_uninstall(void *target, char *error, unsigned long error_size);
extern int dejavu_jit_fs_read(const char *path, void *buffer, unsigned long capacity,
                              unsigned long *size_read);
extern int dejavu_jit_fs_write(const char *path, const void *data, unsigned long size);
extern int dejavu_jit_fs_exists(const char *path);
extern int dejavu_jit_fs_size(const char *path, unsigned long long *size);
extern int dejavu_jit_fs_remove(const char *path);

extern void dejavu_jit_set_error(const char *message);
extern int dejavu_jit_arg_type(const DejavuJitValue *arguments, unsigned long count,
                               unsigned long index);
extern int dejavu_jit_arg_int(const DejavuJitValue *arguments, unsigned long count,
                              unsigned long index, long long *value);
extern int dejavu_jit_arg_boolean(const DejavuJitValue *arguments, unsigned long count,
                                  unsigned long index, int *value);
extern const char *dejavu_jit_arg_string(const DejavuJitValue *arguments, unsigned long count,
                                         unsigned long index, unsigned long *size);
extern void dejavu_jit_return_int(DejavuJitValue *result, long long value);
extern void dejavu_jit_return_boolean(DejavuJitValue *result, int value);
extern void dejavu_jit_return_string(DejavuJitValue *result, const char *text);

const char *dejavu_jit_plugin_name(void);
const char *dejavu_jit_plugin_version(void);
int dejavu_jit_plugin_unload_safe(void);
int dejavu_jit_plugin_capabilities(void);
void dejavu_jit_plugin_control_ready(void);
void dejavu_jit_plugin_unload_prepare(void);
int dejavu_jit_plugin_init(void);
void dejavu_jit_plugin_shutdown(void);
