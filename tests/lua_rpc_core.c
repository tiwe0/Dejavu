#define _POSIX_C_SOURCE 200809L
#include "dejavu.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

struct fixture {
    dejavu_control_session *session;
    unsigned long long retained, target;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int entered, release, polls;
};
static int retain(dejavu_control_session *session, void *opaque, const dejavu_value *args,
                  size_t count, dejavu_value *result, char *error, size_t size) {
    struct fixture *f = opaque;
    (void)result;
    assert(count == 1 && args[0].type == DEJAVU_VALUE_FUNCTION);
    return dejavu_control_session_retain_function(session, args[0].function_value, &f->retained,
                                                  error, size);
}
static int hold(dejavu_control_session *session, void *opaque, const dejavu_value *args,
                size_t count, dejavu_value *result, char *error, size_t size) {
    struct fixture *f = opaque;
    (void)session;
    (void)args;
    (void)count;
    (void)result;
    (void)error;
    (void)size;
    pthread_mutex_lock(&f->mutex);
    f->entered = 1;
    pthread_cond_broadcast(&f->condition);
    while (!f->release)
        pthread_cond_wait(&f->condition, &f->mutex);
    pthread_mutex_unlock(&f->mutex);
    return DEJAVU_OK;
}
static int nested(dejavu_control_session *session, void *opaque, const dejavu_value *args,
                  size_t count, dejavu_value *result, char *error, size_t size) {
    struct fixture *f = opaque;
    dejavu_value invalid = {0}, value;
    (void)args;
    (void)count;
    invalid.type = (enum dejavu_value_type)999;
    assert(dejavu_control_session_invoke(session, f->target, &invalid, 1, &value, error, size) ==
           DEJAVU_ERROR_INVALID_ARGUMENT);
    assert(dejavu_control_session_eval(session, "return 39", &value, error, size) == DEJAVU_OK);
    *result = value;
    return DEJAVU_OK;
}
static void *holder(void *opaque) {
    struct fixture *f = opaque;
    dejavu_value value;
    char error[256];
    assert(dejavu_control_session_eval(f->session, "host.hold()", &value, error, sizeof(error)) ==
           DEJAVU_OK);
    return NULL;
}
static int cancel_wait(void *opaque) {
    struct fixture *f = opaque;
    return ++f->polls >= 4;
}
static int cancel_after_lock(void *opaque) {
    struct fixture *f = opaque;
    return ++f->polls >= 2;
}
static void eval(struct fixture *f, const char *script) {
    dejavu_value value;
    char error[256];
    int status = dejavu_control_session_eval(f->session, script, &value, error, sizeof(error));
    if (status)
        fprintf(stderr, "%s\n", error);
    assert(status == DEJAVU_OK);
    dejavu_value_release(&value);
}
int main(void) {
    struct fixture f = {0};
    dejavu_engine *engine = dejavu_engine_create();
    dejavu_value value, argument = {0};
    char error[256];
    pthread_t thread;
    assert(engine);
    pthread_mutex_init(&f.mutex, NULL);
    pthread_cond_init(&f.condition, NULL);
    assert(dejavu_control_session_create(engine, &f.session, error, sizeof(error)) == DEJAVU_OK);
    assert(dejavu_control_session_register(f.session, "host", "retain", retain, &f, error,
                                           sizeof(error)) == DEJAVU_OK);
    assert(dejavu_control_session_register(f.session, "host", "hold", hold, &f, error,
                                           sizeof(error)) == DEJAVU_OK);
    assert(dejavu_control_session_register(f.session, "host", "nested", nested, &f, error,
                                           sizeof(error)) == DEJAVU_OK);
    f.retained = 123;
    assert(dejavu_control_session_retain_function(f.session, 2, &f.retained, error,
                                                  sizeof(error)) == DEJAVU_ERROR_LUA_RESULT);
    assert(f.retained == 0);
    eval(&f, "host.retain(function(x) return x+2 end); collectgarbage('collect')");
    argument.type = DEJAVU_VALUE_INTEGER;
    argument.integer_value = 40;
    assert(dejavu_control_session_invoke(f.session, f.retained, &argument, 1, &value, error,
                                         sizeof(error)) == DEJAVU_OK);
    assert(value.type == DEJAVU_VALUE_INTEGER && value.integer_value == 42);
    dejavu_control_session_release_function(f.session, f.retained);
    assert(dejavu_control_session_eval(f.session,
                                       "calls=0; return function() calls=calls+1; return 42 end",
                                       &value, error, sizeof(error)) == DEJAVU_OK);
    f.target = value.function_value;
    assert(pthread_create(&thread, NULL, holder, &f) == 0);
    pthread_mutex_lock(&f.mutex);
    while (!f.entered)
        pthread_cond_wait(&f.condition, &f.mutex);
    pthread_mutex_unlock(&f.mutex);
    assert(dejavu_control_session_invoke_cancellable(f.session, f.target, NULL, 0, &value,
                                                     cancel_wait, &f, error,
                                                     sizeof(error)) == DEJAVU_ERROR_CANCELLED);
    assert(f.polls == 4);
    pthread_mutex_lock(&f.mutex);
    f.release = 1;
    pthread_cond_broadcast(&f.condition);
    pthread_mutex_unlock(&f.mutex);
    pthread_join(thread, NULL);
    assert(f.polls == 4);
    f.polls = 0;
    assert(dejavu_control_session_invoke_cancellable(f.session, f.target, NULL, 0, &value,
                                                     cancel_after_lock, &f, error,
                                                     sizeof(error)) == DEJAVU_ERROR_CANCELLED);
    assert(f.polls == 2);
    eval(&f, "assert(calls==0); assert(host.nested(11,22)+3==42)");
    eval(&f,
         "assert(json.encode(9007199254740991)=='9007199254740991'); "
         "assert(json.encode(-9007199254740991)=='-9007199254740991'); "
         "assert(json.decode('9007199254740991')==9007199254740991); "
         "assert(json.decode('-9007199254740991')==-9007199254740991); "
         "assert(json.encode(json.decode('9007199254740991'))=='9007199254740991'); "
         "assert(json.decode('1.25')==1.25); "
         "for _,n in ipairs({9007199254740992,-9007199254740992,math.maxinteger,math.mininteger}) "
         "do "
         "local ok,e=pcall(json.encode,n); assert(not ok and string.find(e,'use strings',1,true)) "
         "end; "
         "for _,s in ipairs({'9007199254740992','9007199254740993','-9007199254740992','1e20'}) do "
         "local ok,e=pcall(json.decode,s); assert(not ok and string.find(e,'use strings',1,true)) "
         "end; "
         "assert(json.encode({})=='{}'); assert(json.encode(json.array())=='[]'); "
         "assert(json.encode(json.decode('[]'))=='[]'); "
         "local v=json.decode([[{\"a\":[null,{},[],\"你好\\uD83D\\uDE00\"]}]]); "
         "assert(v.a[1]==json.null and json.encode(v.a[2])=='{}' and json.encode(v.a[3])=='[]'); "
         "assert(json.decode(json.encode(v)).a[4]=='你好😀'); "
         "local function bad(v) assert(not pcall(json.encode,v)) end; "
         "local c={}; c.x=c; bad(c); bad({[1]=1,[3]=3}); bad({[1]=1,a=2}); "
         "bad(0/0); bad(math.huge); bad(string.char(0)); bad(string.char(255)); "
         "bad({[string.char(0)]=1}); "
         "assert(#json.encode(string.rep('x',1048574))==1048576); "
         "bad(string.rep(string.char(1),200000)); "
         "bad(json.object({1})); bad(json.array({x=1})); bad(string.rep('x',1048577)); "
         "local t={}; local r=t; for i=1,65 do r[1]={}; r=r[1] end; bad(t); "
         "for _,s in ipairs({[[{\"x\":1,\"x\":2}]], [[{\"x\":null,\"\\u0078\":2}]], "
         "'01', '1.', '1e', '-01', string.char(11)..'null', string.char(34,1,34), [[\"\\u0000\"]], "
         "[[\"\\uD800\"]], '1e999', string.char(255), "
         "string.rep('[',65)..string.rep(']',65), string.rep(' ',1048577)}) do assert(not "
         "pcall(json.decode,s)) end; "
         "local many={}; for i=1,65537 do many[i]=false end; bad(many); "
         "assert(not pcall(json.decode,'['..string.rep('null,',65536)..'null]')); "
         "local deep=string.rep('[',64)..string.rep(']',64); "
         "assert(json.encode(json.decode(deep))==deep); "
         "assert(not pcall(json.encode,function() end)); "
         "local t=setmetatable({a=1},{__pairs=function() error('metamethod') "
         "end,__index=function() error('index') end}); "
         "assert(json.encode(t)=='{\"a\":1}')");
    assert(dejavu_control_session_eval(f.session, "return {}", &value, error, sizeof(error)) ==
           DEJAVU_ERROR_LUA_RESULT);
    dejavu_control_session_release_function(f.session, f.target);
    dejavu_control_session_destroy(f.session);
    dejavu_engine_destroy(engine);
    pthread_cond_destroy(&f.condition);
    pthread_mutex_destroy(&f.mutex);
    puts("OK: Lua RPC core retention, cancellation, nested stack, JSON");
    return 0;
}
