-- Device regression: Lua hook.stream_listen/accept/read/write/close roundtrip.
-- Rendered and executed by stream_rpc_test.py (__PORT__ substituted).
local listener = hook.stream_listen("127.0.0.1:__PORT__", 4)
if type(listener) ~= "number" or listener <= 0 then
    return "LISTEN_FAIL:" .. tostring(listener)
end
local connection = 0
for _ = 1, 100 do
    connection = hook.stream_accept(listener, 200)
    if type(connection) == "number" and connection > 0 then
        break
    end
end
if type(connection) ~= "number" or connection <= 0 then
    hook.stream_close(listener)
    return "ACCEPT_FAIL:" .. tostring(connection)
end
local payload = hook.stream_read(connection, 4096, 5000)
if payload == nil then
    hook.stream_close(connection)
    hook.stream_close(listener)
    return "READ_TIMEOUT"
end
local write_status = hook.stream_write(connection, "pong:" .. payload)
hook.stream_close(connection)
hook.stream_close(listener)
return "OK write=" .. tostring(write_status) .. " payload=" .. tostring(payload)
