/* Host-side syntax/ABI check for include/dejavu_jit.h (no TinyCC needed). */
#include "dejavu_jit.h"

static int demo_function(void *session, void *context, const DejavuJitValue *arguments,
                         unsigned long argument_count, DejavuJitValue *result, char *error,
                         unsigned long error_size) {
    long long value = 0;
    (void)session;
    (void)context;
    (void)error;
    (void)error_size;
    if (!dejavu_jit_arg_int(arguments, argument_count, 0, &value)) {
        dejavu_jit_return_string(result, "expected an integer");
        return DEJAVU_JIT_INVALID_ARGUMENT;
    }
    dejavu_jit_return_int(result, value + 1);
    return DEJAVU_JIT_OK;
}

int dejavu_jit_plugin_init(void) {
    char error[128];
    /* Keep the v0.8 stream listen/accept/read ABI referenced so ABI headers and
       host symbol registration cannot drift apart unnoticed. */
    (void)dejavu_jit_stream_listen;
    (void)dejavu_jit_stream_accept;
    (void)dejavu_jit_stream_read;
    (void)dejavu_jit_stream_capture_context;
    (void)dejavu_jit_stream_bind_context;
    return dejavu_jit_register("demo", "increment", demo_function, NULL, error, sizeof(error));
}

const char *dejavu_jit_plugin_name(void) {
    return "demo";
}

const char *dejavu_jit_plugin_version(void) {
    return "1.0.0";
}

int dejavu_jit_plugin_unload_safe(void) {
    return 1;
}

int dejavu_jit_plugin_capabilities(void) {
    return 0;
}

void dejavu_jit_plugin_shutdown(void) {
}
