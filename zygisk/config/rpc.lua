-- Embedded by the agent; the scalar bridge never exposes Lua registry references.
local register, update, unregister, cancelled =
    rpc._register, rpc._update_routes, rpc._unregister, rpc._is_cancelled
local json = json
local business_error = {}
local function strings(value, label)
    assert(type(value) == 'table', label .. ' must be an array')
    local copy, count = json.array(), 0
    for key, item in pairs(value) do
        assert(type(key) == 'number' and key >= 1 and key % 1 == 0 and
               type(item) == 'string' and item ~= '' and not item:find('\0', 1, true),
               label .. ' must contain strings')
        copy[key], count = item, count + 1
    end
    assert(#copy == count, label .. ' must be a dense array')
    return json.encode(copy)
end
rpc.null, rpc.array = json.null, json.array
function rpc.error(code, message, retryable)
    assert(type(code) == 'string' and code ~= '' and type(message) == 'string',
           'rpc.error requires code and message')
    assert(retryable == nil or type(retryable) == 'boolean', 'retryable must be boolean')
    error({tag = business_error, code = code, message = message, retryable = retryable == true}, 0)
end
function rpc.register(spec)
    assert(type(spec) == 'table', 'rpc.register requires a table')
    assert(type(spec.name) == 'string' and type(spec.app) == 'string', 'name and app required')
    assert(type(spec.methods) == 'table', 'methods required')
    assert(spec.endpoint == nil or type(spec.endpoint) == 'string', 'endpoint must be a string')
    local methods, names = {}, json.array()
    for name, callback in pairs(spec.methods) do
        assert(type(name) == 'string' and name ~= '' and not name:find('\0', 1, true) and
               type(callback) == 'function', 'methods must map names to functions')
        methods[name], names[#names + 1] = callback, name
    end
    table.sort(names)
    local function dispatch(params_json, id, route, method, deadline, token)
        local request = {id = id, route = route, method = method,
                         deadline_monotonic_ms = deadline,
                         is_cancelled = function() return cancelled(token) end}
        if request.is_cancelled() then error('cancelled', 0) end
        local ok, value = pcall(function() return methods[method](json.decode(params_json), request) end)
        if ok then return json.encode({ok = true, result = value == nil and json.null or value}) end
        if type(value) == 'table' and value.tag == business_error then
            return json.encode({ok = false, error = {code = value.code, message = value.message,
                                                    retryable = value.retryable}})
        end
        return json.encode({handler_failed = true})
    end
    local handle = register(spec.name, spec.app, strings(spec.routes, 'routes'),
                            json.encode(names), dispatch, spec.endpoint or '')
    local service = {}
    function service:update_routes(routes)
        assert(handle, 'RPC service is unregistered')
        update(handle, strings(routes, 'routes'))
        return true
    end
    function service:unregister()
        if not handle then return true end
        local status = unregister(handle)
        if status == 0 then handle = nil; return true end
        return false, 'busy'
    end
    return service
end
