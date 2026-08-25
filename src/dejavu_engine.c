#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "dejavu.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "libtcc.h"

struct dejavu_symbol {
    char *name;
    const void *address;
};

struct dejavu_engine {
    struct dejavu_symbol *symbols;
    size_t symbol_count;
    size_t symbol_capacity;
};

struct dejavu_module {
    TCCState *state;
    void *memory;
    size_t memory_size;
};

struct dejavu_error_sink {
    char *buffer;
    size_t capacity;
    size_t length;
};

static int is_symbol_name(const char *name) {
    const unsigned char *cursor = (const unsigned char *)name;

    if (cursor == NULL || (!isalpha(*cursor) && *cursor != '_')) {
        return 0;
    }
    for (++cursor; *cursor != '\0'; ++cursor) {
        if (!isalnum(*cursor) && *cursor != '_') {
            return 0;
        }
    }
    return 1;
}

static void clear_error(char *error, size_t error_size) {
    if (error != NULL && error_size != 0) {
        error[0] = '\0';
    }
}

static void append_error(struct dejavu_error_sink *sink, const char *message) {
    size_t available;
    size_t message_length;
    size_t copy_length;

    if (sink->buffer == NULL || sink->capacity == 0 || message == NULL) {
        return;
    }

    if (sink->length != 0 && sink->length + 1 < sink->capacity) {
        sink->buffer[sink->length++] = '\n';
    }
    available = sink->capacity - sink->length - 1;
    message_length = strlen(message);
    copy_length = message_length < available ? message_length : available;
    memcpy(sink->buffer + sink->length, message, copy_length);
    sink->length += copy_length;
    sink->buffer[sink->length] = '\0';
}

static void tcc_error_callback(void *opaque, const char *message) {
    append_error((struct dejavu_error_sink *)opaque, message);
}

static int fail_compile(
    TCCState *state,
    int status,
    struct dejavu_error_sink *sink,
    const char *fallback) {
    if (sink->length == 0) {
        append_error(sink, fallback);
    }
    tcc_delete(state);
    return status;
}

dejavu_engine *dejavu_engine_create(void) {
    return (dejavu_engine *)calloc(1, sizeof(dejavu_engine));
}

void dejavu_engine_destroy(dejavu_engine *engine) {
    size_t index;

    if (engine == NULL) {
        return;
    }
    for (index = 0; index < engine->symbol_count; ++index) {
        free(engine->symbols[index].name);
    }
    free(engine->symbols);
    free(engine);
}

int dejavu_engine_add_symbol(
    dejavu_engine *engine,
    const char *name,
    const void *address) {
    struct dejavu_symbol *symbols;
    char *name_copy;
    size_t index;
    size_t capacity;
    size_t name_size;

    if (engine == NULL || address == NULL) {
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    if (!is_symbol_name(name)) {
        return DEJAVU_ERROR_INVALID_SYMBOL;
    }

    for (index = 0; index < engine->symbol_count; ++index) {
        if (strcmp(engine->symbols[index].name, name) == 0) {
            engine->symbols[index].address = address;
            return DEJAVU_OK;
        }
    }

    if (engine->symbol_count == engine->symbol_capacity) {
        capacity = engine->symbol_capacity == 0 ? 8 : engine->symbol_capacity * 2;
        symbols = (struct dejavu_symbol *)realloc(
            engine->symbols,
            capacity * sizeof(struct dejavu_symbol));
        if (symbols == NULL) {
            return DEJAVU_ERROR_OUT_OF_MEMORY;
        }
        engine->symbols = symbols;
        engine->symbol_capacity = capacity;
    }

    name_size = strlen(name) + 1;
    name_copy = (char *)malloc(name_size);
    if (name_copy == NULL) {
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    memcpy(name_copy, name, name_size);
    engine->symbols[engine->symbol_count].name = name_copy;
    engine->symbols[engine->symbol_count].address = address;
    ++engine->symbol_count;
    return DEJAVU_OK;
}

int dejavu_engine_compile(
    const dejavu_engine *engine,
    const char *source,
    dejavu_module **module,
    char *error,
    size_t error_size) {
    struct dejavu_error_sink sink = {error, error_size, 0};
    dejavu_module *new_module;
    TCCState *state;
    void *memory;
    long page_size;
    int relocation_size;
    size_t allocation_size;
    size_t index;

    clear_error(error, error_size);
    if (engine == NULL || source == NULL || module == NULL) {
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    *module = NULL;

    state = tcc_new();
    if (state == NULL) {
        append_error(&sink, "unable to create TinyCC state");
        return DEJAVU_ERROR_TCC_CREATE;
    }

    tcc_set_error_func(state, &sink, tcc_error_callback);
    tcc_set_options(state, "-nostdlib");
    if (tcc_set_output_type(state, TCC_OUTPUT_MEMORY) < 0) {
        return fail_compile(state, DEJAVU_ERROR_TCC_OUTPUT, &sink,
                            "unable to select in-memory output");
    }

    for (index = 0; index < engine->symbol_count; ++index) {
        if (tcc_add_symbol(state,
                           engine->symbols[index].name,
                           engine->symbols[index].address) < 0) {
            return fail_compile(state, DEJAVU_ERROR_TCC_SYMBOL, &sink,
                                "unable to register host symbol");
        }
    }

    if (tcc_compile_string(state, source) < 0) {
        return fail_compile(state, DEJAVU_ERROR_TCC_COMPILE, &sink,
                            "C compilation failed");
    }
    relocation_size = tcc_relocate(state, NULL);
    if (relocation_size < 0) {
        return fail_compile(state, DEJAVU_ERROR_TCC_RELOCATE, &sink,
                            "C relocation failed");
    }

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        page_size = 4096;
    }
    allocation_size = ((size_t)relocation_size + (size_t)page_size - 1) /
                      (size_t)page_size * (size_t)page_size;
    memory = mmap(NULL,
                  allocation_size,
                  PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS,
                  -1,
                  0);
    if (memory == MAP_FAILED) {
        return fail_compile(state, DEJAVU_ERROR_OUT_OF_MEMORY, &sink,
                            "unable to allocate code pages");
    }
    if (tcc_relocate(state, memory) < 0) {
        munmap(memory, allocation_size);
        return fail_compile(state, DEJAVU_ERROR_TCC_RELOCATE, &sink,
                            "unable to protect code pages");
    }

    new_module = (dejavu_module *)malloc(sizeof(dejavu_module));
    if (new_module == NULL) {
        munmap(memory, allocation_size);
        return fail_compile(state, DEJAVU_ERROR_OUT_OF_MEMORY, &sink,
                            "unable to allocate module");
    }
    new_module->state = state;
    new_module->memory = memory;
    new_module->memory_size = allocation_size;
    *module = new_module;
    return DEJAVU_OK;
}

void *dejavu_module_symbol(dejavu_module *module, const char *name) {
    if (module == NULL || name == NULL) {
        return NULL;
    }
    return tcc_get_symbol(module->state, name);
}

int dejavu_module_call(dejavu_module *module, int *result) {
    void *symbol;
    int (*entry)(void) = NULL;

    if (module == NULL || result == NULL) {
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }

    symbol = dejavu_module_symbol(module, DEJAVU_ENTRY_SYMBOL);
    if (symbol == NULL || sizeof(entry) != sizeof(symbol)) {
        return DEJAVU_ERROR_TCC_SYMBOL;
    }
    memcpy(&entry, &symbol, sizeof(entry));
    *result = entry();
    return DEJAVU_OK;
}

void dejavu_module_destroy(dejavu_module *module) {
    if (module == NULL) {
        return;
    }
    tcc_delete(module->state);
    munmap(module->memory, module->memory_size);
    free(module);
}
