/* JIT plugin that installs a host-tracked native hook and is unload-safe. */

static int (*original_atoi)(const char *value);
static void *hooked_target;

static int hook_atoi(const char *value) {
    return original_atoi(value);
}

static int install_hook(void *session, void *context, const DejavuJitValue *arguments,
                        unsigned long argument_count, DejavuJitValue *result, char *error,
                        unsigned long error_size) {
    void *target;
    int status;
    (void)session;
    (void)context;
    (void)arguments;
    (void)argument_count;
    target = dejavu_jit_module_resolve("libc.so", "atoi");
    if (target == NULL) {
        dejavu_jit_return_string(result, "atoi target not found");
        return 2;
    }
    status = dejavu_jit_hook_install(target, (void *)hook_atoi, (void **)&original_atoi, error,
                                     error_size);
    if (status != 0) {
        dejavu_jit_return_string(result, "hook install failed");
        return 2;
    }
    hooked_target = target;
    dejavu_jit_return_boolean(result, 1);
    return 0;
}

static int hook_info(void *session, void *context, const DejavuJitValue *arguments,
                     unsigned long argument_count, DejavuJitValue *result, char *error,
                     unsigned long error_size) {
    (void)session;
    (void)context;
    (void)arguments;
    (void)argument_count;
    (void)error;
    (void)error_size;
    dejavu_jit_return_boolean(result, original_atoi != NULL ? 1 : 0);
    return 0;
}

const char *dejavu_jit_plugin_name(void) {
    return "hellohookjit";
}

const char *dejavu_jit_plugin_version(void) {
    return "1.0.0";
}

int dejavu_jit_plugin_unload_safe(void) {
    return 1;
}

int dejavu_jit_plugin_capabilities(void) {
    return 4; /* DEJAVU_PLUGIN_CAP_HOOK */
}

int dejavu_jit_plugin_init(void) {
    char error[128];
    if (dejavu_jit_register("hooktest", "install", install_hook, NULL, error, sizeof(error)) != 0) {
        dejavu_jit_set_error("failed to register hooktest.install");
        return 1;
    }
    if (dejavu_jit_register("hooktest", "installed", hook_info, NULL, error, sizeof(error)) != 0) {
        dejavu_jit_set_error("failed to register hooktest.installed");
        return 1;
    }
    return 0;
}

void dejavu_jit_plugin_unload_prepare(void) {
    char error[128];
    if (hooked_target != NULL) {
        dejavu_jit_hook_uninstall(hooked_target, error, sizeof(error));
        hooked_target = NULL;
    }
}

void dejavu_jit_plugin_shutdown(void) {
}
