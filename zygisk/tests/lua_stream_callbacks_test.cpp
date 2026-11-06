#include "lua_stream_wait.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct dejavu_control_session;
enum { DEJAVU_OK = 0, DEJAVU_ERROR_INVALID_ARGUMENT = 1, DEJAVU_ERROR_OUT_OF_MEMORY = 2 };
enum { DEJAVU_VALUE_NIL = 0, DEJAVU_VALUE_INTEGER = 2, DEJAVU_VALUE_STRING = 4 };
struct dejavu_value {
    int type = DEJAVU_VALUE_NIL;
    int owned = 0;
    long long integer_value = 0;
    const char *string_value = nullptr;
    size_t string_size = 0;
};
namespace dejavu_hook_manager_internal {
constexpr uint64_t kLuaStreamOwnerToken = 0x4C75615F5354524DULL;
}
struct Backend {
    unsigned int accepts = 0, reads = 0;
    int last_timeout = -1;
    int stream_accept(int, int timeout, uint64_t owner) {
        assert(owner == dejavu_hook_manager_internal::kLuaStreamOwnerToken);
        ++accepts;
        last_timeout = timeout;
        return 0;
    }
    int stream_read(int, void *, size_t, int timeout, size_t *size, uint64_t owner) {
        assert(owner == dejavu_hook_manager_internal::kLuaStreamOwnerToken);
        ++reads;
        last_timeout = timeout;
        *size = 0;
        return 0;
    }
};
struct HookManager {
    struct Impl {
        Backend *plugins;
        static int lua_stream_accept(dejavu_control_session *, void *, const dejavu_value *, size_t,
                                     dejavu_value *, char *, size_t);
        static int lua_stream_read(dejavu_control_session *, void *, const dejavu_value *, size_t,
                                   dejavu_value *, char *, size_t);
    };
};
static void copy_error(char *buffer, size_t size, const std::string &error) {
    std::snprintf(buffer, size, "%s", error.c_str());
}
static bool set_owned_string(dejavu_value *result, const std::string &text) {
    char *copy = static_cast<char *>(std::malloc(text.size() + 1));
    if (copy == nullptr)
        return false;
    std::memcpy(copy, text.data(), text.size());
    copy[text.size()] = 0;
    result->type = DEJAVU_VALUE_STRING;
    result->owned = 1;
    result->string_value = copy;
    result->string_size = text.size();
    return true;
}
// The script appends these two exact production callbacks, avoiding unrelated
// JNI/Android dependencies. Only their transport backend returns immediately.
#include "lua_stream_callbacks.inc"

int main() {
    Backend backend;
    HookManager::Impl manager{&backend};
    dejavu_value arguments[3];
    arguments[0].type = DEJAVU_VALUE_INTEGER;
    arguments[0].integer_value = 1;
    arguments[1].type = DEJAVU_VALUE_INTEGER;
    arguments[1].integer_value = 64;
    arguments[2].type = DEJAVU_VALUE_INTEGER;
    dejavu_value result;
    char error[512]{};
    for (long long invalid : {-1LL, -2LL, 10001LL, 600000LL}) {
        arguments[1].integer_value = invalid;
        assert(HookManager::Impl::lua_stream_accept(nullptr, &manager, arguments, 2, &result, error,
                                                    sizeof(error)) != DEJAVU_OK);
        arguments[1].integer_value = 64;
        arguments[2].integer_value = invalid;
        assert(HookManager::Impl::lua_stream_read(nullptr, &manager, arguments, 3, &result, error,
                                                  sizeof(error)) != DEJAVU_OK);
    }
    assert(backend.accepts == 0 && backend.reads == 0);
    for (long long valid : {0LL, 25LL, 10000LL}) {
        arguments[1].integer_value = valid;
        assert(HookManager::Impl::lua_stream_accept(nullptr, &manager, arguments, 2, &result, error,
                                                    sizeof(error)) == DEJAVU_OK);
        assert(backend.last_timeout == valid);
        arguments[1].integer_value = 64;
        arguments[2].integer_value = valid;
        assert(HookManager::Impl::lua_stream_read(nullptr, &manager, arguments, 3, &result, error,
                                                  sizeof(error)) == DEJAVU_OK);
        assert(backend.last_timeout == valid && result.type == DEJAVU_VALUE_NIL);
    }
    {
        LuaStreamWaitScope exhausted(LuaStreamWaitScope::Clock::now() - std::chrono::seconds(26));
        assert(HookManager::Impl::lua_stream_accept(nullptr, &manager, arguments, 1, &result, error,
                                                    sizeof(error)) != DEJAVU_OK);
        assert(std::strstr(error, "budget exhausted") != nullptr);
        assert(HookManager::Impl::lua_stream_read(nullptr, &manager, arguments, 2, &result, error,
                                                  sizeof(error)) != DEJAVU_OK);
    }
    {
        LuaStreamWaitScope next_request;
        assert(HookManager::Impl::lua_stream_accept(nullptr, &manager, arguments, 1, &result, error,
                                                    sizeof(error)) == DEJAVU_OK);
        assert(backend.last_timeout == 5000);
        assert(HookManager::Impl::lua_stream_read(nullptr, &manager, arguments, 2, &result, error,
                                                  sizeof(error)) == DEJAVU_OK);
        assert(backend.last_timeout == 1000);
    }
    {
        LuaStreamWaitScope almost_spent(LuaStreamWaitScope::Clock::now() -
                                        std::chrono::seconds(24));
        LuaStreamWaitScope reentrant;
        assert(HookManager::Impl::lua_stream_accept(nullptr, &manager, arguments, 1, &result, error,
                                                    sizeof(error)) == DEJAVU_OK);
        assert(backend.last_timeout >= 0 && backend.last_timeout <= 1000);
    }
    std::puts("OK: production Lua stream callbacks reject unbounded waits and recover in the next "
              "request");
}
