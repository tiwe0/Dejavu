/* TinyCC JIT plugin example: no headers, no libc; host ABI comes from the prelude. */

static unsigned long jit_strlen(const char *text) {
    unsigned long size = 0;
    while (text[size] != '\0') {
        ++size;
    }
    return size;
}

static char *jit_copy(const char *text) {
    unsigned long size = jit_strlen(text);
    char *copy = (char *)dejavu_jit_alloc(size + 1);
    unsigned long index;
    if (copy == NULL) {
        return NULL;
    }
    for (index = 0; index < size; ++index) {
        copy[index] = text[index];
    }
    copy[size] = '\0';
    return copy;
}

static void put_string(DejavuJitValue *result, const char *text) {
    result->type = 4; /* DEJAVU_VALUE_STRING */
    result->owned = 1;
    result->string_value = jit_copy(text);
    result->string_size = jit_strlen(text);
    result->integer_value = 0;
    result->number_value = 0;
    result->handle_value = NULL;
    result->function_value = 0;
    result->host_function = NULL;
    result->host_context = NULL;
}

static int greet(void *session, void *context, const DejavuJitValue *arguments,
                 unsigned long argument_count, DejavuJitValue *result, char *error,
                 unsigned long error_size) {
    (void)session;
    (void)context;
    (void)arguments;
    (void)argument_count;
    (void)error;
    (void)error_size;
    put_string(result, "hello from a TinyCC JIT plugin");
    return result->string_value != NULL ? 0 : 2;
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
        put_string(result, "JIT plugin: mem=ok module=ok");
    } else if (mem_ok == 0) {
        put_string(result, "JIT plugin: mem=ok module=failed");
    } else if (getpid_symbol != NULL) {
        put_string(result, "JIT plugin: mem=failed module=ok");
    } else {
        put_string(result, "JIT plugin: mem=failed module=failed");
    }
    return result->string_value != NULL ? 0 : 2;
}

const char *dejavu_jit_plugin_name(void) {
    return "hellojit";
}

const char *dejavu_jit_plugin_version(void) {
    return "1.0.0";
}

int dejavu_jit_plugin_unload_safe(void) {
    return 1;
}

int dejavu_jit_plugin_init(void) {
    char error[128];
    if (dejavu_jit_register("hellojit", "greet", greet, NULL, error, sizeof(error)) != 0) {
        return 1;
    }
    if (dejavu_jit_register("hellojit", "info", info, NULL, error, sizeof(error)) != 0) {
        return 1;
    }
    return 0;
}

void dejavu_jit_plugin_shutdown(void) {
}
