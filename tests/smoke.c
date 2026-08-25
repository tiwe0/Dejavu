#include "dejavu.h"

#include <stdio.h>
#include <string.h>

static int host_add(int left, int right) {
    return left + right;
}

static int lua_host_add(
    dejavu_control_session *session,
    void *context,
    const dejavu_value *arguments,
    size_t argument_count,
    dejavu_value *result,
    char *error,
    size_t error_size) {
    (void)session;
    (void)context;
    (void)error;
    (void)error_size;
    if (argument_count != 2 ||
        arguments[0].type != DEJAVU_VALUE_INTEGER ||
        arguments[1].type != DEJAVU_VALUE_INTEGER) {
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
    result->type = DEJAVU_VALUE_INTEGER;
    result->integer_value = arguments[0].integer_value + arguments[1].integer_value;
    return DEJAVU_OK;
}

static int check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        return 0;
    }
    return 1;
}

int main(void) {
    static const char direct_source[] =
        "int dejavu_entry(void) { return 6 * 7; }";
    static const char host_source[] =
        "extern int host_add(int, int);"
        "int dejavu_entry(void) { return host_add(20, 22); }";
    static const char unresolved_source[] =
        "extern int forbidden(void);"
        "int dejavu_entry(void) { return forbidden(); }";
    static const char lua_script[] =
        "local run = dejavu.compile([["
        "  extern int host_add(int, int);"
        "  int dejavu_entry(void) { return host_add(19, 23); }"
        "]]) "
        "return run()";
    dejavu_engine *engine;
    dejavu_module *module = NULL;
    dejavu_control_session *session = NULL;
    dejavu_value value;
    dejavu_value argument;
    unsigned long long callback = 0;
    char error[512];
    int result = 0;
    int status;

    if (!check(strcmp(dejavu_version(), "0.1.0") == 0, "Dejavu version")) {
        return 1;
    }
    engine = dejavu_engine_create();
    if (!check(engine != NULL, "engine creation")) {
        return 1;
    }

    status = dejavu_engine_compile(engine, direct_source, &module, error, sizeof(error));
    if (!check(status == DEJAVU_OK, error) ||
        !check(dejavu_module_call(module, &result) == DEJAVU_OK, "direct call") ||
        !check(result == 42, "direct result")) {
        dejavu_module_destroy(module);
        dejavu_engine_destroy(engine);
        return 1;
    }
    dejavu_module_destroy(module);
    module = NULL;

    status = dejavu_engine_add_symbol(engine, "host_add", (const void *)&host_add);
    if (!check(status == DEJAVU_OK, "host symbol registration")) {
        dejavu_engine_destroy(engine);
        return 1;
    }
    status = dejavu_engine_compile(engine, host_source, &module, error, sizeof(error));
    if (!check(status == DEJAVU_OK, error) ||
        !check(dejavu_module_call(module, &result) == DEJAVU_OK, "host call") ||
        !check(result == 42, "host result")) {
        dejavu_module_destroy(module);
        dejavu_engine_destroy(engine);
        return 1;
    }
    dejavu_module_destroy(module);
    module = NULL;

    status = dejavu_engine_compile(
        engine, unresolved_source, &module, error, sizeof(error));
    if (!check(status == DEJAVU_ERROR_TCC_RELOCATE, "unresolved symbol status") ||
        !check(module == NULL, "unresolved symbol module") ||
        !check(strstr(error, "forbidden") != NULL, "unresolved symbol error")) {
        dejavu_module_destroy(module);
        dejavu_engine_destroy(engine);
        return 1;
    }

    status = dejavu_control_run(engine, lua_script, &result, error, sizeof(error));
    if (!check(status == DEJAVU_OK, error) ||
        !check(result == 42, "Lua control result")) {
        dejavu_engine_destroy(engine);
        return 1;
    }

    status = dejavu_control_session_create(engine, &session, error, sizeof(error));
    if (!check(status == DEJAVU_OK, error)) {
        dejavu_engine_destroy(engine);
        return 1;
    }
    status = dejavu_control_session_register(
        session, "host", "add", lua_host_add, NULL, error, sizeof(error));
    if (!check(status == DEJAVU_OK, error)) {
        dejavu_control_session_destroy(session);
        dejavu_engine_destroy(engine);
        return 1;
    }
    status = dejavu_control_session_eval(
        session,
        "counter = (counter or 0) + 1; return function(x) return host.add(counter, x) end",
        &value,
        error,
        sizeof(error));
    if (!check(status == DEJAVU_OK, error) ||
        !check(value.type == DEJAVU_VALUE_FUNCTION, "persistent Lua function result")) {
        dejavu_value_release(&value);
        dejavu_control_session_destroy(session);
        dejavu_engine_destroy(engine);
        return 1;
    }
    callback = value.function_value;
    memset(&argument, 0, sizeof(argument));
    argument.type = DEJAVU_VALUE_INTEGER;
    argument.integer_value = 41;
    memset(&value, 0, sizeof(value));
    status = dejavu_control_session_invoke(
        session, callback, &argument, 1, &value, error, sizeof(error));
    if (!check(status == DEJAVU_OK, error) ||
        !check(value.type == DEJAVU_VALUE_INTEGER, "persistent Lua callback type") ||
        !check(value.integer_value == 42, "persistent Lua callback result")) {
        dejavu_value_release(&value);
        dejavu_control_session_release_function(session, callback);
        dejavu_control_session_destroy(session);
        dejavu_engine_destroy(engine);
        return 1;
    }
    dejavu_value_release(&value);
    dejavu_control_session_release_function(session, callback);
    dejavu_control_session_destroy(session);

    if (!check(dejavu_smoke() == DEJAVU_OK, "public smoke")) {
        dejavu_engine_destroy(engine);
        return 1;
    }
    dejavu_engine_destroy(engine);
    puts("OK: engine, symbol allowlist, persistent Lua control");
    return 0;
}
