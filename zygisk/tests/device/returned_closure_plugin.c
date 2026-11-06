/* Device-only lifecycle fixture. TinyCC prepends dejavu_jit.h. No app data I/O. */
static int calls;

static int leaf(void *session, void *context, const DejavuJitValue *arguments,
                unsigned long count, DejavuJitValue *result, char *error,
                unsigned long error_size) {
    (void)session; (void)arguments; (void)count; (void)error; (void)error_size;
    if (context != &calls) return DEJAVU_JIT_ERROR;
    dejavu_jit_return_int(result, ++calls);
    return DEJAVU_JIT_OK;
}

static int nested(void *session, void *context, const DejavuJitValue *arguments,
                  unsigned long count, DejavuJitValue *result, char *error,
                  unsigned long error_size) {
    (void)session; (void)context; (void)arguments; (void)count; (void)error; (void)error_size;
    result->type = 7; /* DEJAVU_VALUE_HOST_FUNCTION, public value ABI. */
    result->host_function = (void *)leaf;
    result->host_context = &calls;
    return DEJAVU_JIT_OK;
}

static int make(void *session, void *context, const DejavuJitValue *arguments,
                unsigned long count, DejavuJitValue *result, char *error,
                unsigned long error_size) {
    (void)session; (void)context; (void)arguments; (void)count; (void)error; (void)error_size;
    result->type = 7;
    result->host_function = (void *)nested;
    result->host_context = NULL;
    return DEJAVU_JIT_OK;
}

const char *dejavu_jit_plugin_name(void) { return "closurejit"; }
int dejavu_jit_plugin_unload_safe(void) { return 1; }
int dejavu_jit_plugin_capabilities(void) { return 0; }
int dejavu_jit_plugin_init(void) {
    char error[128];
    return dejavu_jit_register("closurejit", "make", make, NULL, error, sizeof(error));
}
