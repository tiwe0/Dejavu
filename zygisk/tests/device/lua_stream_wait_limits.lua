-- Memory-only device regression. Run another RPC after this script returns.
-- Set stream_wait_test_budget=true for the approximately 25-second shared
-- evaluation-budget check; the quick parameter/finite-wait check is default.
local port = stream_wait_test_port or 18767
assert(type(port) == "number" and port % 1 == 0 and port > 0 and port <= 65535)
local listener = hook.stream_listen("127.0.0.1:" .. port, 1)
local function rejected(callback, ...)
    local ok, message = pcall(callback, ...)
    assert(not ok and tostring(message):find("timeout", 1, true), tostring(message))
end
local function accept_timeout(timeout)
    local connection = hook.stream_accept(listener, timeout)
    if connection > 0 then
        hook.stream_close(connection)
        error("unexpected test client")
    end
    assert(connection == 0, "listener unexpectedly failed")
end
local ok, result = pcall(function()
    rejected(hook.stream_accept, listener, -1)
    rejected(hook.stream_accept, listener, 10001)
    rejected(hook.stream_read, listener, 64, -1)
    rejected(hook.stream_read, listener, 64, 10001)
    accept_timeout(0)
    accept_timeout(20)
    if stream_wait_test_budget then
        local waited, failure = pcall(function()
            for _ = 1, 5 do accept_timeout(10000) end
        end)
        assert(not waited and tostring(failure):find("budget exhausted", 1, true), tostring(failure))
    end
    return "OK: Lua wait limits; finite waits returned; shared_budget=" .. tostring(not not stream_wait_test_budget)
end)
local closed, close_result = pcall(hook.stream_close, listener)
if not ok then error(result) end
assert(closed and close_result, "test listener cleanup failed")
return result
