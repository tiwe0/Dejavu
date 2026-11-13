/* White-box allocator fault injection without adding production test hooks. */
#include "../src/dejavu_lua.c"
#include <assert.h>
#include <stdio.h>

struct fault_fixture {
    lua_Alloc original;
    void *original_context;
    int failures;
    int mode;
    int unlocked;
    unsigned long long target;
    unsigned long long retained[4096];
    size_t retained_count;
    dejavu_control_session *session;
};
static void *fail_growth(void *opaque, void *pointer, size_t old_size, size_t new_size) {
    struct fault_fixture *fixture = opaque;
    if (new_size > old_size || (pointer == NULL && new_size != 0)) {
        ++fixture->failures;
        return NULL;
    }
    return fixture->original(fixture->original_context, pointer, old_size, new_size);
}
static int dummy_host(dejavu_control_session *session, void *context, const dejavu_value *args,
                      size_t count, dejavu_value *result, char *error, size_t size) {
    (void)session;
    (void)context;
    (void)args;
    (void)count;
    (void)result;
    (void)error;
    (void)size;
    return DEJAVU_OK;
}
static int trigger(dejavu_control_session *session, void *opaque, const dejavu_value *args,
                   size_t count, dejavu_value *result, char *error, size_t size) {
    struct fault_fixture *fixture = opaque;
    int status = DEJAVU_OK;
    assert(count == 1 && args[0].type == DEJAVU_VALUE_FUNCTION);
    fixture->original = lua_getallocf(session->state, &fixture->original_context);
    lua_setallocf(session->state, fail_growth, fixture);
    if (fixture->mode == 0) {
        while (status == DEJAVU_OK && fixture->retained_count < 4096) {
            unsigned long long retained = 0;
            status = dejavu_control_session_retain_function(session, args[0].function_value,
                                                            &retained, error, size);
            if (status == DEJAVU_OK)
                fixture->retained[fixture->retained_count++] = retained;
            else
                assert(retained == 0);
        }
    } else {
        dejavu_value argument = {0}, value;
        char text[8192];
        memset(text, 'x', sizeof(text));
        if (fixture->mode == 1) {
            argument.type = DEJAVU_VALUE_STRING;
            argument.string_value = text;
            argument.string_size = sizeof(text);
        } else {
            argument.type = DEJAVU_VALUE_HOST_FUNCTION;
            argument.host_function = dummy_host;
        }
        status = dejavu_control_session_invoke_cancellable(session, fixture->target, &argument, 1,
                                                           &value, NULL, NULL, error, size);
    }
    lua_setallocf(session->state, fixture->original, fixture->original_context);
    result->type = DEJAVU_VALUE_INTEGER;
    result->integer_value = status;
    return DEJAVU_OK;
}
static void *check_unlocked(void *opaque) {
    struct fault_fixture *fixture = opaque;
    fixture->unlocked = pthread_mutex_trylock(&fixture->session->mutex) == 0;
    if (fixture->unlocked)
        pthread_mutex_unlock(&fixture->session->mutex);
    return NULL;
}
int main(void) {
    struct fault_fixture fixture = {0};
    dejavu_engine *engine = dejavu_engine_create();
    dejavu_value value;
    pthread_t thread;
    char error[256];
    assert(engine);
    assert(dejavu_control_session_create(engine, &fixture.session, error, sizeof(error)) ==
           DEJAVU_OK);
    assert(dejavu_control_session_register(fixture.session, "host", "fault", trigger, &fixture,
                                           error, sizeof(error)) == DEJAVU_OK);
    assert(dejavu_control_session_eval(fixture.session, "return function(x) return 42 end", &value,
                                       error, sizeof(error)) == DEJAVU_OK);
    fixture.target = value.function_value;
    for (fixture.mode = 0; fixture.mode < 3; ++fixture.mode) {
        int base = lua_gettop(fixture.session->state);
        fixture.failures = 0;
        assert(dejavu_control_session_eval(fixture.session,
                                           "return host.fault(function() return 7 end)", &value,
                                           error, sizeof(error)) == DEJAVU_OK);
        assert(value.type == DEJAVU_VALUE_INTEGER &&
               value.integer_value == DEJAVU_ERROR_OUT_OF_MEMORY);
        assert(fixture.failures > 0);
        assert(lua_gettop(fixture.session->state) == base);
        assert(pthread_create(&thread, NULL, check_unlocked, &fixture) == 0);
        pthread_join(thread, NULL);
        assert(fixture.unlocked);
        assert(dejavu_control_session_invoke(fixture.session, fixture.target, NULL, 0, &value,
                                             error, sizeof(error)) == DEJAVU_OK);
        assert(value.type == DEJAVU_VALUE_INTEGER && value.integer_value == 42);
    }
    for (size_t i = 0; i < fixture.retained_count; ++i)
        dejavu_control_session_release_function(fixture.session, fixture.retained[i]);
    dejavu_control_session_release_function(fixture.session, fixture.target);
    dejavu_control_session_destroy(fixture.session);
    dejavu_engine_destroy(engine);
    puts("OK: allocator failures preserve nested Lua locks and stack");
    return 0;
}
