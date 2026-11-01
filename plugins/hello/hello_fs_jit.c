/* JIT plugin exercising the host fs service (no libc/stdio available). */

static int fs_write(void *session, void *context, const DejavuJitValue *arguments,
                    unsigned long argument_count, DejavuJitValue *result, char *error,
                    unsigned long error_size) {
    unsigned long path_size = 0;
    unsigned long data_size = 0;
    const char *path;
    const char *data;
    (void)session;
    (void)context;
    (void)error;
    (void)error_size;
    path = dejavu_jit_arg_string(arguments, argument_count, 0, &path_size);
    data = dejavu_jit_arg_string(arguments, argument_count, 1, &data_size);
    if (path == NULL || data == NULL) {
        dejavu_jit_return_boolean(result, 0);
        return 1;
    }
    dejavu_jit_return_boolean(result, dejavu_jit_fs_write(path, data, data_size) == 0 ? 1 : 0);
    return 0;
}

static int fs_read(void *session, void *context, const DejavuJitValue *arguments,
                   unsigned long argument_count, DejavuJitValue *result, char *error,
                   unsigned long error_size) {
    char buffer[1024];
    unsigned long path_size = 0;
    unsigned long read_size = 0;
    const char *path;
    (void)session;
    (void)context;
    (void)error;
    (void)error_size;
    path = dejavu_jit_arg_string(arguments, argument_count, 0, &path_size);
    if (path == NULL) {
        dejavu_jit_return_string(result, "path required");
        return 1;
    }
    if (dejavu_jit_fs_read(path, buffer, sizeof(buffer) - 1, &read_size) != 0) {
        dejavu_jit_return_string(result, "read failed");
        return 1;
    }
    buffer[read_size < sizeof(buffer) ? read_size : sizeof(buffer) - 1] = '\0';
    dejavu_jit_return_string(result, buffer);
    return 0;
}

static int fs_exists(void *session, void *context, const DejavuJitValue *arguments,
                     unsigned long argument_count, DejavuJitValue *result, char *error,
                     unsigned long error_size) {
    unsigned long path_size = 0;
    const char *path;
    (void)session;
    (void)context;
    (void)error;
    (void)error_size;
    path = dejavu_jit_arg_string(arguments, argument_count, 0, &path_size);
    dejavu_jit_return_boolean(result, path != NULL ? dejavu_jit_fs_exists(path) : 0);
    return 0;
}

static int fs_remove(void *session, void *context, const DejavuJitValue *arguments,
                     unsigned long argument_count, DejavuJitValue *result, char *error,
                     unsigned long error_size) {
    unsigned long path_size = 0;
    const char *path;
    (void)session;
    (void)context;
    (void)error;
    (void)error_size;
    path = dejavu_jit_arg_string(arguments, argument_count, 0, &path_size);
    dejavu_jit_return_boolean(result, path != NULL && dejavu_jit_fs_remove(path) == 0 ? 1 : 0);
    return 0;
}

const char *dejavu_jit_plugin_name(void) {
    return "hellofsjit";
}

const char *dejavu_jit_plugin_version(void) {
    return "1.0.0";
}

int dejavu_jit_plugin_unload_safe(void) {
    return 1;
}

int dejavu_jit_plugin_capabilities(void) {
    return 8; /* DEJAVU_PLUGIN_CAP_FS */
}

int dejavu_jit_plugin_init(void) {
    char error[128];
    if (dejavu_jit_register("fstest", "write", fs_write, NULL, error, sizeof(error)) != 0) {
        dejavu_jit_set_error("failed to register fstest.write");
        return 1;
    }
    if (dejavu_jit_register("fstest", "read", fs_read, NULL, error, sizeof(error)) != 0) {
        dejavu_jit_set_error("failed to register fstest.read");
        return 1;
    }
    if (dejavu_jit_register("fstest", "exists", fs_exists, NULL, error, sizeof(error)) != 0) {
        dejavu_jit_set_error("failed to register fstest.exists");
        return 1;
    }
    if (dejavu_jit_register("fstest", "remove", fs_remove, NULL, error, sizeof(error)) != 0) {
        dejavu_jit_set_error("failed to register fstest.remove");
        return 1;
    }
    return 0;
}

void dejavu_jit_plugin_shutdown(void) {
}
