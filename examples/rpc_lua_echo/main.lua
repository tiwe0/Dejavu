local service
local requests = 0
local plugin = {}

function plugin.start(ctx)
    assert(type(rpc) == "table" and type(rpc.register) == "function",
           "Update the Dejavu module to a version with Lua RPC support")
    assert(service == nil, "RPC service is already registered")
    service = rpc.register {
        name = "rpc_lua_echo",
        app = "example.lua_rpc",
        routes = {"example.lua.echo"},
        methods = {
            echo = function(params, request)
                if request.is_cancelled() then
                    rpc.error("example.cancelled", "Request cancelled", false)
                end
                requests = requests + 1
                return params
            end,
            status = function()
                return {service = "rpc_lua_echo", version = "1.0.0", requests = requests}
            end,
            fail = function()
                rpc.error("example.rejected", "Example business rejection", false)
            end,
        },
    }
    return true
end

function plugin.stop(ctx)
    if service ~= nil then
        local stopped = service:unregister()
        if not stopped then return false end
        service = nil
    end
    return true
end

function plugin.status(ctx)
    return {running = service ~= nil, requests = requests}
end

return plugin
