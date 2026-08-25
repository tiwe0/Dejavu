#ifndef DEJAVU_H
#define DEJAVU_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define DEJAVU_API __attribute__((visibility("default")))
#else
#define DEJAVU_API
#endif

#define DEJAVU_VERSION_MAJOR 0
#define DEJAVU_VERSION_MINOR 1
#define DEJAVU_VERSION_PATCH 0
#define DEJAVU_ENTRY_SYMBOL "dejavu_entry"

typedef struct dejavu_engine dejavu_engine;
typedef struct dejavu_module dejavu_module;
typedef struct dejavu_control_session dejavu_control_session;

enum dejavu_value_type {
    DEJAVU_VALUE_NIL = 0,
    DEJAVU_VALUE_BOOLEAN = 1,
    DEJAVU_VALUE_INTEGER = 2,
    DEJAVU_VALUE_NUMBER = 3,
    DEJAVU_VALUE_STRING = 4,
    DEJAVU_VALUE_HANDLE = 5,
    DEJAVU_VALUE_FUNCTION = 6,
    DEJAVU_VALUE_HOST_FUNCTION = 7,
};

typedef struct dejavu_value dejavu_value;

typedef int (*dejavu_host_function)(
    dejavu_control_session *session,
    void *context,
    const dejavu_value *arguments,
    size_t argument_count,
    dejavu_value *result,
    char *error,
    size_t error_size);

struct dejavu_value {
    enum dejavu_value_type type;
    int owned;
    int boolean_value;
    long long integer_value;
    double number_value;
    const char *string_value;
    size_t string_size;
    void *handle_value;
    unsigned long long function_value;
    dejavu_host_function host_function;
    void *host_context;
};

enum dejavu_status {
    DEJAVU_OK = 0,
    DEJAVU_ERROR_INVALID_ARGUMENT = 1,
    DEJAVU_ERROR_OUT_OF_MEMORY = 2,
    DEJAVU_ERROR_INVALID_SYMBOL = 3,
    DEJAVU_ERROR_LUA_CREATE = 10,
    DEJAVU_ERROR_LUA_LOAD = 11,
    DEJAVU_ERROR_LUA_EXECUTE = 12,
    DEJAVU_ERROR_LUA_RESULT = 13,
    DEJAVU_ERROR_TCC_CREATE = 20,
    DEJAVU_ERROR_TCC_OUTPUT = 21,
    DEJAVU_ERROR_TCC_COMPILE = 22,
    DEJAVU_ERROR_TCC_RELOCATE = 23,
    DEJAVU_ERROR_TCC_SYMBOL = 24,
    DEJAVU_ERROR_TCC_RESULT = 25,
};

DEJAVU_API const char *dejavu_version(void);
DEJAVU_API const char *dejavu_lua_version(void);
DEJAVU_API const char *dejavu_tcc_version(void);

DEJAVU_API dejavu_engine *dejavu_engine_create(void);
DEJAVU_API void dejavu_engine_destroy(dejavu_engine *engine);

/* Registers or replaces a host symbol made available to generated code. */
DEJAVU_API int dejavu_engine_add_symbol(
    dejavu_engine *engine,
    const char *name,
    const void *address);

/*
 * Compiles freestanding C. The returned module owns its executable code.
 * The engine must not be mutated concurrently with this call.
 */
DEJAVU_API int dejavu_engine_compile(
    const dejavu_engine *engine,
    const char *source,
    dejavu_module **module,
    char *error,
    size_t error_size);

DEJAVU_API void *dejavu_module_symbol(dejavu_module *module, const char *name);
DEJAVU_API int dejavu_module_call(dejavu_module *module, int *result);
DEJAVU_API void dejavu_module_destroy(dejavu_module *module);

/*
 * Persistent Lua control session. All operations on one session are
 * serialized. Function values returned to host callbacks are registry
 * references and must eventually be released.
 */
DEJAVU_API int dejavu_control_session_create(
    dejavu_engine *engine,
    dejavu_control_session **session,
    char *error,
    size_t error_size);
DEJAVU_API void dejavu_control_session_destroy(dejavu_control_session *session);
DEJAVU_API int dejavu_control_session_register(
    dejavu_control_session *session,
    const char *module_name,
    const char *function_name,
    dejavu_host_function function,
    void *context,
    char *error,
    size_t error_size);
DEJAVU_API int dejavu_control_session_eval(
    dejavu_control_session *session,
    const char *script,
    dejavu_value *result,
    char *error,
    size_t error_size);
DEJAVU_API int dejavu_control_session_invoke(
    dejavu_control_session *session,
    unsigned long long function,
    const dejavu_value *arguments,
    size_t argument_count,
    dejavu_value *result,
    char *error,
    size_t error_size);
DEJAVU_API void dejavu_control_session_release_function(
    dejavu_control_session *session,
    unsigned long long function);
DEJAVU_API void dejavu_value_release(dejavu_value *value);

/* Runs a Lua control script. dejavu.compile(C_SOURCE) returns a Lua function. */
DEJAVU_API int dejavu_control_run(
    dejavu_engine *engine,
    const char *script,
    int *result,
    char *error,
    size_t error_size);

/* Each smoke function writes 42 on success. */
DEJAVU_API int dejavu_smoke_lua(int *result);
DEJAVU_API int dejavu_smoke_tcc(int *result);

/* Runs both smoke checks and returns the first non-zero status. */
DEJAVU_API int dejavu_smoke(void);

#ifdef __cplusplus
}
#endif

#endif
