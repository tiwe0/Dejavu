#include "lua_stream_wait.h"
#include <cassert>
#include <climits>
#include <cstdio>
#include <string>
using Scope = LuaStreamWaitScope;
using namespace std::chrono_literals;

int main() {
    const Scope::Clock::time_point epoch{};
    int timeout = -1;
    std::string error;
    for (long long invalid : {-1LL, -2LL, 10001LL, LLONG_MAX, LLONG_MIN})
        assert(!Scope::limit(invalid, &timeout, &error, epoch));
    assert(Scope::limit(0, &timeout, &error, epoch) && timeout == 0);
    assert(Scope::limit(10000, &timeout, &error, epoch) && timeout == 10000);
    {
        Scope evaluation(epoch);
        assert(Scope::limit(10000, &timeout, &error, epoch + 19s) && timeout == 6000);
        {
            Scope reentrant(epoch + 20s);
            assert(Scope::limit(10000, &timeout, &error, epoch + 24s) && timeout == 1000);
        }
        assert(Scope::limit(5000, &timeout, &error, epoch + 24999ms) && timeout == 1);
        assert(!Scope::limit(1, &timeout, &error, epoch + 24999500us));
        assert(Scope::limit(0, &timeout, &error, epoch + 24999500us) && timeout == 0);
        assert(!Scope::limit(1, &timeout, &error, epoch + 25s));
        assert(error.find("budget exhausted") != std::string::npos);
        assert(!Scope::limit(0, &timeout, &error, epoch + 25s));
    }
    {
        Scope next_request(epoch + 1h);
        assert(Scope::limit(5000, &timeout, &error, epoch + 1h) && timeout == 5000);
    }
    assert(Scope::limit(10000, &timeout, &error, epoch + 2h) && timeout == 10000);
    std::puts("OK: Lua stream limits, shared reentrant budget and request reset");
}
