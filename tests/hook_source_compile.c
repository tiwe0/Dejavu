#include "dejavu.h"

#include <stdio.h>
#include <stdlib.h>

/* Resolve helpers without calling them: this test exercises the real TinyCC
 * compiler/relocator and rejects implicit libc dependencies in hook sources. */
static void helper_stub(void) {
}

int main(int argc, char **argv) {
    if (argc < 3) {
        return 2;
    }
    FILE *file = fopen(argv[1], "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        return 2;
    }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 2;
    }
    char *source = malloc((size_t)length + 1);
    if (source == NULL) {
        fclose(file);
        return 2;
    }
    if (fread(source, 1, (size_t)length, file) != (size_t)length) {
        free(source);
        fclose(file);
        return 2;
    }
    source[length] = '\0';
    fclose(file);
    dejavu_engine *engine = dejavu_engine_create();
    if (engine == NULL) {
        free(source);
        return 2;
    }
    for (int i = 2; i < argc; ++i) {
        if (dejavu_engine_add_symbol(engine, argv[i], (const void *)helper_stub) != DEJAVU_OK) {
            dejavu_engine_destroy(engine);
            free(source);
            return 2;
        }
    }
    dejavu_module *module = NULL;
    char error[2048];
    int status = dejavu_engine_compile(engine, source, &module, error, sizeof(error));
    free(source);
    if (status != DEJAVU_OK) {
        fprintf(stderr, "hook source failed to compile/relocate: %s\n", error);
        dejavu_engine_destroy(engine);
        return 1;
    }
    int valid = dejavu_module_symbol(module, "after_hook") != NULL;
    dejavu_module_destroy(module);
    dejavu_engine_destroy(engine);
    if (!valid) {
        fprintf(stderr, "missing after_hook entry point\n");
        return 1;
    }
    puts("OK: hook source compiles/relocates with only the actual Hook ABI allowlist");
    return 0;
}
