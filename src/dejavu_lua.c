#include "dejavu.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#define DEJAVU_MODULE_METATABLE "dejavu.module"

struct lua_dejavu_module {
    dejavu_module *module;
};

struct dejavu_host_binding {
    dejavu_host_function function;
    void *context;
    struct dejavu_host_binding *next;
};

struct dejavu_control_session {
    lua_State *state;
    dejavu_engine *engine;
    pthread_mutex_t mutex;
    struct dejavu_host_binding *bindings;
};

static void copy_error(char *error, size_t error_size, const char *message) {
    size_t length;

    if (error == NULL || error_size == 0) {
        return;
    }
    if (message == NULL) {
        error[0] = '\0';
        return;
    }
    length = strlen(message);
    if (length >= error_size) {
        length = error_size - 1;
    }
    memcpy(error, message, length);
    error[length] = '\0';
}

static int lua_module_gc(lua_State *state) {
    struct lua_dejavu_module *holder = (struct lua_dejavu_module *)
        luaL_checkudata(state, 1, DEJAVU_MODULE_METATABLE);

    dejavu_module_destroy(holder->module);
    holder->module = NULL;
    return 0;
}

static int lua_module_call(lua_State *state) {
    struct lua_dejavu_module *holder = (struct lua_dejavu_module *)
        luaL_checkudata(state, lua_upvalueindex(1), DEJAVU_MODULE_METATABLE);
    int result;
    int status;

    if (lua_gettop(state) != 0) {
        return luaL_error(state, "compiled function takes no arguments");
    }
    status = dejavu_module_call(holder->module, &result);
    if (status != DEJAVU_OK) {
        return luaL_error(state, "compiled module has no int dejavu_entry(void)");
    }
    lua_pushinteger(state, result);
    return 1;
}

static int lua_dejavu_compile(lua_State *state) {
    dejavu_engine *engine = (dejavu_engine *)lua_touserdata(state, lua_upvalueindex(1));
    struct lua_dejavu_module *holder;
    const char *source;
    char error[512];
    int status;

    source = luaL_checkstring(state, 1);
    holder = (struct lua_dejavu_module *)lua_newuserdatauv(state, sizeof(*holder), 0);
    holder->module = NULL;
    luaL_setmetatable(state, DEJAVU_MODULE_METATABLE);

    status = dejavu_engine_compile(engine, source, &holder->module, error, sizeof(error));
    if (status != DEJAVU_OK) {
        return luaL_error(state, "C compile failed: %s", error[0] != '\0' ? error : "unknown error");
    }

    lua_pushvalue(state, -1);
    lua_pushcclosure(state, lua_module_call, 1);
    lua_remove(state, -2);
    return 1;
}

static void open_control_libraries(lua_State *state) {
    luaL_requiref(state, LUA_GNAME, luaopen_base, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(state, 1);

    lua_pushnil(state);
    lua_setglobal(state, "dofile");
    lua_pushnil(state);
    lua_setglobal(state, "loadfile");
}

static void clear_value(dejavu_value *value) {
    if (value != NULL) {
        memset(value, 0, sizeof(*value));
        value->type = DEJAVU_VALUE_NIL;
    }
}

void dejavu_value_release(dejavu_value *value) {
    if (value == NULL) {
        return;
    }
    if (value->type == DEJAVU_VALUE_STRING && value->owned && value->string_value != NULL) {
        free((void *)value->string_value);
    }
    clear_value(value);
}

static int copy_lua_value(
    lua_State *state,
    int index,
    int copy_string,
    dejavu_value *value,
    char *error,
    size_t error_size) {
    size_t string_size;
    const char *string_value;

    clear_value(value);
    switch (lua_type(state, index)) {
        case LUA_TNIL:
            return DEJAVU_OK;
        case LUA_TBOOLEAN:
            value->type = DEJAVU_VALUE_BOOLEAN;
            value->boolean_value = lua_toboolean(state, index);
            return DEJAVU_OK;
        case LUA_TNUMBER:
            if (lua_isinteger(state, index)) {
                value->type = DEJAVU_VALUE_INTEGER;
                value->integer_value = (long long)lua_tointegerx(state, index, NULL);
            } else {
                value->type = DEJAVU_VALUE_NUMBER;
                value->number_value = (double)lua_tonumberx(state, index, NULL);
            }
            return DEJAVU_OK;
        case LUA_TSTRING:
            string_value = lua_tolstring(state, index, &string_size);
            value->type = DEJAVU_VALUE_STRING;
            value->string_size = string_size;
            if (!copy_string) {
                value->string_value = string_value;
                return DEJAVU_OK;
            }
            {
                char *copy = (char *)malloc(string_size + 1);
                if (copy == NULL) {
                    copy_error(error, error_size, "unable to copy Lua string result");
                    return DEJAVU_ERROR_OUT_OF_MEMORY;
                }
                memcpy(copy, string_value, string_size);
                copy[string_size] = '\0';
                value->string_value = copy;
                value->owned = 1;
            }
            return DEJAVU_OK;
        case LUA_TLIGHTUSERDATA:
            value->type = DEJAVU_VALUE_HANDLE;
            value->handle_value = lua_touserdata(state, index);
            return DEJAVU_OK;
        case LUA_TFUNCTION:
            lua_pushvalue(state, index);
            value->type = DEJAVU_VALUE_FUNCTION;
            value->function_value = (unsigned long long)luaL_ref(state, LUA_REGISTRYINDEX);
            value->owned = 1;
            return DEJAVU_OK;
        default:
            copy_error(error, error_size, "unsupported Lua value type");
            return DEJAVU_ERROR_LUA_RESULT;
    }
}

static int lua_host_dispatch(lua_State *state);

static int push_value(lua_State *state, const dejavu_value *value) {
    struct dejavu_host_binding *binding;

    if (value == NULL || value->type == DEJAVU_VALUE_NIL) {
        lua_pushnil(state);
        return DEJAVU_OK;
    }
    switch (value->type) {
        case DEJAVU_VALUE_BOOLEAN:
            lua_pushboolean(state, value->boolean_value);
            return DEJAVU_OK;
        case DEJAVU_VALUE_INTEGER:
            lua_pushinteger(state, (lua_Integer)value->integer_value);
            return DEJAVU_OK;
        case DEJAVU_VALUE_NUMBER:
            lua_pushnumber(state, (lua_Number)value->number_value);
            return DEJAVU_OK;
        case DEJAVU_VALUE_STRING:
            lua_pushlstring(
                state,
                value->string_value != NULL ? value->string_value : "",
                value->string_value != NULL ? value->string_size : 0);
            return DEJAVU_OK;
        case DEJAVU_VALUE_HANDLE:
            lua_pushlightuserdata(state, value->handle_value);
            return DEJAVU_OK;
        case DEJAVU_VALUE_FUNCTION:
            lua_rawgeti(state, LUA_REGISTRYINDEX, (lua_Integer)value->function_value);
            return lua_isfunction(state, -1) ? DEJAVU_OK : DEJAVU_ERROR_LUA_RESULT;
        case DEJAVU_VALUE_HOST_FUNCTION:
            if (value->host_function == NULL) {
                return DEJAVU_ERROR_INVALID_ARGUMENT;
            }
            binding = (struct dejavu_host_binding *)lua_newuserdatauv(
                state, sizeof(*binding), 0);
            binding->function = value->host_function;
            binding->context = value->host_context;
            binding->next = NULL;
            lua_pushcclosure(state, lua_host_dispatch, 1);
            return DEJAVU_OK;
        default:
            return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
}

static int lua_host_dispatch(lua_State *state) {
    struct dejavu_host_binding *binding = (struct dejavu_host_binding *)
        lua_touserdata(state, lua_upvalueindex(1));
    dejavu_control_session *session;
    dejavu_value *arguments = NULL;
    dejavu_value result;
    char error[512];
    int argument_count;
    int index;
    int status;

    if (binding == NULL || binding->function == NULL) {
        return luaL_error(state, "invalid Dejavu host function");
    }
    lua_getfield(state, LUA_REGISTRYINDEX, "dejavu.control.session");
    session = (dejavu_control_session *)lua_touserdata(state, -1);
    lua_pop(state, 1);
    if (session == NULL) {
        return luaL_error(state, "missing Dejavu control session");
    }

    argument_count = lua_gettop(state);
    if (argument_count > 0) {
        arguments = (dejavu_value *)calloc((size_t)argument_count, sizeof(*arguments));
        if (arguments == NULL) {
            return luaL_error(state, "unable to allocate host arguments");
        }
    }
    error[0] = '\0';
    clear_value(&result);
    for (index = 0; index < argument_count; ++index) {
        status = copy_lua_value(
            state, index + 1, 0, &arguments[index], error, sizeof(error));
        if (status != DEJAVU_OK) {
            free(arguments);
            return luaL_error(state, "%s", error[0] != '\0' ? error : "invalid host argument");
        }
    }

    status = binding->function(
        session,
        binding->context,
        arguments,
        (size_t)argument_count,
        &result,
        error,
        sizeof(error));
    free(arguments);
    if (status != DEJAVU_OK) {
        dejavu_value_release(&result);
        return luaL_error(state, "%s", error[0] != '\0' ? error : "host function failed");
    }
    status = push_value(state, &result);
    dejavu_value_release(&result);
    if (status != DEJAVU_OK) {
        return luaL_error(state, "invalid host function result");
    }
    return 1;
}

int dejavu_control_session_create(
    dejavu_engine *engine,
    dejavu_control_session **session,
    char *error,
    size_t error_size) {
    dejavu_control_session *created;
    pthread_mutexattr_t attributes;

    copy_error(error, error_size, NULL);
    if (engine == NULL || session == NULL) {
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    *session = NULL;
    created = (dejavu_control_session *)calloc(1, sizeof(*created));
    if (created == NULL) {
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    if (pthread_mutexattr_init(&attributes) != 0) {
        free(created);
        copy_error(error, error_size, "unable to create control-session mutex");
        return DEJAVU_ERROR_LUA_CREATE;
    }
    if (pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE) != 0 ||
        pthread_mutex_init(&created->mutex, &attributes) != 0) {
        pthread_mutexattr_destroy(&attributes);
        free(created);
        copy_error(error, error_size, "unable to create control-session mutex");
        return DEJAVU_ERROR_LUA_CREATE;
    }
    pthread_mutexattr_destroy(&attributes);

    created->state = luaL_newstate();
    if (created->state == NULL) {
        pthread_mutex_destroy(&created->mutex);
        free(created);
        copy_error(error, error_size, "unable to create Lua state");
        return DEJAVU_ERROR_LUA_CREATE;
    }
    created->engine = engine;
    open_control_libraries(created->state);
    if (luaL_newmetatable(created->state, DEJAVU_MODULE_METATABLE)) {
        lua_pushcfunction(created->state, lua_module_gc);
        lua_setfield(created->state, -2, "__gc");
    }
    lua_pop(created->state, 1);

    lua_newtable(created->state);
    lua_pushlightuserdata(created->state, engine);
    lua_pushcclosure(created->state, lua_dejavu_compile, 1);
    lua_setfield(created->state, -2, "compile");
    lua_setglobal(created->state, "dejavu");

    lua_pushlightuserdata(created->state, created);
    lua_setfield(created->state, LUA_REGISTRYINDEX, "dejavu.control.session");
    *session = created;
    return DEJAVU_OK;
}

void dejavu_control_session_destroy(dejavu_control_session *session) {
    struct dejavu_host_binding *binding;
    struct dejavu_host_binding *next;

    if (session == NULL) {
        return;
    }
    pthread_mutex_lock(&session->mutex);
    lua_close(session->state);
    session->state = NULL;
    binding = session->bindings;
    while (binding != NULL) {
        next = binding->next;
        free(binding);
        binding = next;
    }
    pthread_mutex_unlock(&session->mutex);
    pthread_mutex_destroy(&session->mutex);
    free(session);
}

int dejavu_control_session_register(
    dejavu_control_session *session,
    const char *module_name,
    const char *function_name,
    dejavu_host_function function,
    void *context,
    char *error,
    size_t error_size) {
    struct dejavu_host_binding *binding;

    copy_error(error, error_size, NULL);
    if (session == NULL || module_name == NULL || module_name[0] == '\0' ||
        function_name == NULL || function_name[0] == '\0' || function == NULL) {
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    binding = (struct dejavu_host_binding *)calloc(1, sizeof(*binding));
    if (binding == NULL) {
        return DEJAVU_ERROR_OUT_OF_MEMORY;
    }
    binding->function = function;
    binding->context = context;

    pthread_mutex_lock(&session->mutex);
    lua_getglobal(session->state, module_name);
    if (!lua_istable(session->state, -1)) {
        lua_pop(session->state, 1);
        lua_newtable(session->state);
        lua_pushvalue(session->state, -1);
        lua_setglobal(session->state, module_name);
    }
    lua_pushlightuserdata(session->state, binding);
    lua_pushcclosure(session->state, lua_host_dispatch, 1);
    lua_setfield(session->state, -2, function_name);
    lua_pop(session->state, 1);
    binding->next = session->bindings;
    session->bindings = binding;
    pthread_mutex_unlock(&session->mutex);
    return DEJAVU_OK;
}

int dejavu_control_session_eval(
    dejavu_control_session *session,
    const char *script,
    dejavu_value *result,
    char *error,
    size_t error_size) {
    int status;

    copy_error(error, error_size, NULL);
    clear_value(result);
    if (session == NULL || script == NULL || result == NULL) {
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&session->mutex);
    status = luaL_loadbufferx(session->state, script, strlen(script), "dejavu-control", "t");
    if (status != LUA_OK) {
        copy_error(error, error_size, lua_tostring(session->state, -1));
        lua_pop(session->state, 1);
        pthread_mutex_unlock(&session->mutex);
        return DEJAVU_ERROR_LUA_LOAD;
    }
    status = lua_pcallk(session->state, 0, 1, 0, 0, NULL);
    if (status != LUA_OK) {
        copy_error(error, error_size, lua_tostring(session->state, -1));
        lua_pop(session->state, 1);
        pthread_mutex_unlock(&session->mutex);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    status = copy_lua_value(session->state, -1, 1, result, error, error_size);
    lua_pop(session->state, 1);
    pthread_mutex_unlock(&session->mutex);
    return status;
}

int dejavu_control_session_invoke(
    dejavu_control_session *session,
    unsigned long long function,
    const dejavu_value *arguments,
    size_t argument_count,
    dejavu_value *result,
    char *error,
    size_t error_size) {
    size_t index;
    int status;

    copy_error(error, error_size, NULL);
    clear_value(result);
    if (session == NULL || function == 0 || result == NULL ||
        (argument_count != 0 && arguments == NULL)) {
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&session->mutex);
    lua_rawgeti(session->state, LUA_REGISTRYINDEX, (lua_Integer)function);
    if (!lua_isfunction(session->state, -1)) {
        lua_pop(session->state, 1);
        pthread_mutex_unlock(&session->mutex);
        copy_error(error, error_size, "invalid Lua function reference");
        return DEJAVU_ERROR_LUA_RESULT;
    }
    for (index = 0; index < argument_count; ++index) {
        status = push_value(session->state, &arguments[index]);
        if (status != DEJAVU_OK) {
            lua_settop(session->state, 0);
            pthread_mutex_unlock(&session->mutex);
            copy_error(error, error_size, "invalid Lua callback argument");
            return status;
        }
    }
    status = lua_pcallk(session->state, (int)argument_count, 1, 0, 0, NULL);
    if (status != LUA_OK) {
        copy_error(error, error_size, lua_tostring(session->state, -1));
        lua_pop(session->state, 1);
        pthread_mutex_unlock(&session->mutex);
        return DEJAVU_ERROR_LUA_EXECUTE;
    }
    status = copy_lua_value(session->state, -1, 1, result, error, error_size);
    lua_pop(session->state, 1);
    pthread_mutex_unlock(&session->mutex);
    return status;
}

void dejavu_control_session_release_function(
    dejavu_control_session *session,
    unsigned long long function) {
    if (session == NULL || function == 0) {
        return;
    }
    pthread_mutex_lock(&session->mutex);
    luaL_unref(session->state, LUA_REGISTRYINDEX, (int)function);
    pthread_mutex_unlock(&session->mutex);
}

int dejavu_control_run(
    dejavu_engine *engine,
    const char *script,
    int *result,
    char *error,
    size_t error_size) {
    dejavu_control_session *session;
    dejavu_value value;
    int status;

    copy_error(error, error_size, NULL);
    if (engine == NULL || script == NULL || result == NULL) {
        return DEJAVU_ERROR_INVALID_ARGUMENT;
    }

    status = dejavu_control_session_create(engine, &session, error, error_size);
    if (status != DEJAVU_OK) {
        return status;
    }
    status = dejavu_control_session_eval(session, script, &value, error, error_size);
    if (status != DEJAVU_OK) {
        dejavu_control_session_destroy(session);
        return status;
    }
    if (value.type != DEJAVU_VALUE_INTEGER) {
        copy_error(error, error_size, "control script must return an integer");
        dejavu_value_release(&value);
        dejavu_control_session_destroy(session);
        return DEJAVU_ERROR_LUA_RESULT;
    }

    *result = (int)value.integer_value;
    dejavu_value_release(&value);
    dejavu_control_session_destroy(session);
    return DEJAVU_OK;
}
