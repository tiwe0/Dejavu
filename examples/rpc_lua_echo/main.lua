local service
local requests = 0
local errors = 0
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
            ping = function(params, request)
                if request.is_cancelled() then
                    rpc.error("example.cancelled", "Request cancelled", false)
                end
                return {
                    pong = true,
                    service = "rpc_lua_echo",
                    route = request.route,
                    requests = requests,
                }
            end,
            status = function()
                return {
                    service = "rpc_lua_echo",
                    version = "1.1.0",
                    requests = requests,
                    errors = errors,
                }
            end,
            reset_stats = function()
                requests = 0
                errors = 0
                return {reset = true}
            end,
            fail = function()
                errors = errors + 1
                rpc.error("example.rejected", "Example business rejection", false)
            end,
        },
    }
    if type(ctx) == "table" and type(ctx.track_rpc) == "function" then
        ctx.track_rpc(service)
    end
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
    return {running = service ~= nil, requests = requests, errors = errors}
end

plugin.actions = {
    self_check = function(ctx)
        return service ~= nil
    end,
    reset_stats = function(ctx)
        requests = 0
        errors = 0
        return true
    end,
}

return plugin
