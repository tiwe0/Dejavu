#pragma once

#include <stddef.h>

#include "dejavu.h"

struct DejavuRuntime {
    void *handle;
    decltype(&dejavu_version) version;
    decltype(&dejavu_lua_version) lua_version;
    decltype(&dejavu_tcc_version) tcc_version;
    decltype(&dejavu_engine_create) engine_create;
    decltype(&dejavu_engine_destroy) engine_destroy;
    decltype(&dejavu_engine_add_symbol) engine_add_symbol;
    decltype(&dejavu_engine_compile) engine_compile;
    decltype(&dejavu_module_symbol) module_symbol;
    decltype(&dejavu_module_call) module_call;
    decltype(&dejavu_module_destroy) module_destroy;
    decltype(&dejavu_control_session_create) control_session_create;
    decltype(&dejavu_control_session_destroy) control_session_destroy;
    decltype(&dejavu_control_session_register) control_session_register;
    decltype(&dejavu_control_session_eval) control_session_eval;
    decltype(&dejavu_control_session_invoke) control_session_invoke;
    decltype(&dejavu_control_session_release_function) control_session_release_function;
    decltype(&dejavu_value_release) value_release;
    decltype(&dejavu_control_run) control_run;
    decltype(&dejavu_smoke) smoke;
};

bool dejavu_runtime_load(
    int module_dir_fd,
    DejavuRuntime *runtime,
    char *error,
    size_t error_size);
void dejavu_runtime_unload(DejavuRuntime *runtime);
