/* TinyCC JIT plugin example: no headers, no libc; host ABI comes from the prelude. */

static int greet(void *session, void *context, const DejavuJitValue *arguments,
                 unsigned long argument_count, DejavuJitValue *result, char *error,
                 unsigned long error_size) {
    (void)session;
    (void)context;
    (void)arguments;
    (void)argument_count;
    (void)error;
    (void)error_size;
    dejavu_jit_return_string(result, "hello from a TinyCC JIT plugin");
    return result->type == 4 ? DEJAVU_JIT_OK : DEJAVU_JIT_ERROR;
}

static int add(void *session, void *context, const DejavuJitValue *arguments,
               unsigned long argument_count, DejavuJitValue *result, char *error,
               unsigned long error_size) {
    long long left = 0;
    long long right = 0;
    (void)session;
    (void)context;
    (void)error;
    (void)error_size;
    if (!dejavu_jit_arg_int(arguments, argument_count, 0, &left) ||
        !dejavu_jit_arg_int(arguments, argument_count, 1, &right)) {
        dejavu_jit_return_string(result, "add expects two integers");
        return DEJAVU_JIT_INVALID_ARGUMENT;
    }
    dejavu_jit_return_int(result, left + right);
    return DEJAVU_JIT_OK;
}

static int info(void *session, void *context, const DejavuJitValue *arguments,
                unsigned long argument_count, DejavuJitValue *result, char *error,
                unsigned long error_size) {
    unsigned long long start = 0;
    unsigned long long end = 0;
    unsigned int prot = 0;
    int mem_ok;
    void *getpid_symbol;
    (void)session;
    (void)context;
    (void)arguments;
    (void)argument_count;
    (void)error;
    (void)error_size;
    mem_ok = dejavu_jit_mem_query((unsigned long long)(unsigned long)&start, &start, &end, &prot);
    getpid_symbol = dejavu_jit_module_resolve("libc.so", "getpid");
    dejavu_jit_log(4, "hello_jit: info called");
    if (mem_ok == 0 && getpid_symbol != NULL) {
        dejavu_jit_return_string(result, "JIT plugin: mem=ok module=ok");
    } else if (mem_ok == 0) {
        dejavu_jit_return_string(result, "JIT plugin: mem=ok module=failed");
    } else if (getpid_symbol != NULL) {
        dejavu_jit_return_string(result, "JIT plugin: mem=failed module=ok");
    } else {
        dejavu_jit_return_string(result, "JIT plugin: mem=failed module=failed");
    }
    return result->type == 4 ? DEJAVU_JIT_OK : DEJAVU_JIT_ERROR;
}

const char *dejavu_jit_plugin_name(void) {
    return "hellojit";
}

const char *dejavu_jit_plugin_version(void) {
    return "1.1.0";
}

int dejavu_jit_plugin_unload_safe(void) {
    return 1;
}

int dejavu_jit_plugin_init(void) {
    char error[128];
    if (dejavu_jit_register("hellojit", "greet", greet, NULL, error, sizeof(error)) != 0) {
        dejavu_jit_set_error("failed to register hellojit.greet");
        return 1;
    }
    if (dejavu_jit_register("hellojit", "add", add, NULL, error, sizeof(error)) != 0) {
        dejavu_jit_set_error("failed to register hellojit.add");
        return 1;
    }
    if (dejavu_jit_register("hellojit", "info", info, NULL, error, sizeof(error)) != 0) {
        dejavu_jit_set_error("failed to register hellojit.info");
        return 1;
    }
    return 0;
}

void dejavu_jit_plugin_shutdown(void) {
}
