#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/*
 * Simulates an execmem-restricted SELinux domain (e.g. a hardened Android ROM)
 * by denying any mprotect() that requests PROT_EXEC. The engine must report a
 * normal error instead of terminating the host process (TCC's default behavior
 * for fatal relocation errors before patches/tinycc-runtime-safety.patch).
 */
#include "dejavu.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

int __real_mprotect(void *address, size_t length, int protection);
int __wrap_mprotect(void *address, size_t length, int protection) {
    if ((protection & PROT_EXEC) != 0) {
        errno = EACCES;
        return -1;
    }
    return __real_mprotect(address, length, protection);
}

int main(void) {
    static const char source[] = "int dejavu_entry(void) { return 42; }";
    dejavu_engine *engine;
    dejavu_module *module = NULL;
    char error[512];
    int status;

    engine = dejavu_engine_create();
    if (engine == NULL) {
        fprintf(stderr, "FAIL: unable to create engine\n");
        return 1;
    }
    error[0] = '\0';
    status = dejavu_engine_compile(engine, source, &module, error, sizeof(error));
    if (status != DEJAVU_ERROR_TCC_RELOCATE || module != NULL) {
        fprintf(
            stderr,
            "FAIL: expected a relocate error with no module, got status=%d module=%p error='%s'\n",
            status, (void *)module, error);
        dejavu_module_destroy(module);
        dejavu_engine_destroy(engine);
        return 1;
    }
    if (error[0] == '\0' ||
        (strstr(error, "mprotect") == NULL && strstr(error, "protect") == NULL)) {
        fprintf(stderr, "FAIL: expected an mprotect-related error message, got '%s'\n", error);
        dejavu_engine_destroy(engine);
        return 1;
    }
    dejavu_engine_destroy(engine);
    puts("OK: execmem denial is reported as an error instead of aborting");
    return 0;
}
