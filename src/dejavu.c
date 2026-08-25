#include "dejavu.h"

#include "lua.h"

#ifndef DEJAVU_TCC_VERSION
#define DEJAVU_TCC_VERSION "unknown"
#endif

#define DEJAVU_STRINGIFY_INNER(value) #value
#define DEJAVU_STRINGIFY(value) DEJAVU_STRINGIFY_INNER(value)
#define DEJAVU_VERSION_STRING \
    DEJAVU_STRINGIFY(DEJAVU_VERSION_MAJOR) "." \
    DEJAVU_STRINGIFY(DEJAVU_VERSION_MINOR) "." \
    DEJAVU_STRINGIFY(DEJAVU_VERSION_PATCH)

const char *dejavu_version(void) {
    return DEJAVU_VERSION_STRING;
}

const char *dejavu_lua_version(void) {
    return LUA_RELEASE;
}

const char *dejavu_tcc_version(void) {
    return DEJAVU_TCC_VERSION;
}

int dejavu_smoke_lua(int *result) {
    static const char script[] =
        "local run = dejavu.compile([["
        "int dejavu_entry(void) { return 6 * 7; }"
        "]]) "
        "return run()";
    dejavu_engine *engine;
    char error[128];
    int status;

    if (result == NULL) {
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    engine = dejavu_engine_create();
    if (engine == NULL) {
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    status = dejavu_control_run(engine, script, result, error, sizeof(error));
    dejavu_engine_destroy(engine);
    if (status != DEJAVU_OK || *result != 42) {
        return status != DEJAVU_OK ? status : DEJAVU_ERROR_LUA_RESULT;
    }
    return DEJAVU_OK;
}

int dejavu_smoke_tcc(int *result) {
    static const char source[] = "int dejavu_entry(void) { return 6 * 7; }";
    dejavu_engine *engine;
    dejavu_module *module = NULL;
    char error[128];
    int status;

    if (result == NULL) {
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    engine = dejavu_engine_create();
    if (engine == NULL) {
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    status = dejavu_engine_compile(engine, source, &module, error, sizeof(error));
    if (status == DEJAVU_OK) {
        status = dejavu_module_call(module, result);
    }
    dejavu_module_destroy(module);
    dejavu_engine_destroy(engine);
    if (status != DEJAVU_OK || *result != 42) {
        return status != DEJAVU_OK ? status : DEJAVU_ERROR_TCC_RESULT;
    }
    return DEJAVU_OK;
}

int dejavu_smoke(void) {
    int result;
    int status = dejavu_smoke_lua(&result);

    if (status != DEJAVU_OK) {
        return status;
    }
    return dejavu_smoke_tcc(&result);
}
