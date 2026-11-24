-- Embedded by the agent after host namespaces (java, hook, native, rpc) are registered.
-- 1) Wraps java.* JNI GlobalRef handles in GC-managed + <close>-capable refs with java.scope().
-- 2) Provides dejavu.scope() for deterministic LIFO cleanup of hooks, RPC services, streams, and Java refs.

local HANDLE_KEY = {}
local live_refs = setmetatable({}, { __mode = "v" })
local scope_stack = {}

local function returns_java_object(sig)
    if type(sig) ~= "string" then
        return false
    end
    local ret = sig:match("%)(.+)$")
    if not ret or ret == "Ljava/lang/String;" then
        return false
    end
    local prefix = ret:sub(1, 1)
    return prefix == "L" or prefix == "["
end

local function install_java_resource_guards()
    if type(java) ~= "table" or type(java.release) ~= "function" then
        return
    end
    if rawget(java, "_managed_refs") then
        return
    end

    local raw_invoke = java.invoke
    local raw_new = java.new
    local raw_load_dex = java.load_dex
    local raw_release = java.release

    local java_ref_methods = {}
    local java_ref_mt

    local function state_of(value)
        if type(value) ~= "table" then
            return nil
        end
        return rawget(value, HANDLE_KEY)
    end

    local function unwrap_arg(value)
        local st = state_of(value)
        if st ~= nil then
            assert(st.id ~= nil, "Java object handle has already been released")
            return st.id
        end
        return value
    end

    local function release_state(st, propagate_error)
        local id = st.id
        if id == nil then
            return true
        end
        st.id = nil
        st.owned = false
        if live_refs[id] ~= nil and state_of(live_refs[id]) == st then
            live_refs[id] = nil
        end
        if propagate_error then
            return raw_release(id)
        end
        pcall(raw_release, id)
        return true
    end

    function java_ref_methods:id()
        local st = state_of(self)
        return st and st.id or nil
    end

    function java_ref_methods:is_alive()
        local st = state_of(self)
        return st ~= nil and st.id ~= nil
    end

    function java_ref_methods:release()
        local st = assert(state_of(self), "expected a Java object handle")
        return release_state(st, true)
    end

    function java_ref_methods:detach()
        local st = assert(state_of(self), "expected a Java object handle")
        local id = assert(st.id, "Java object handle has already been released")
        st.id = nil
        st.owned = false
        if live_refs[id] == self then
            live_refs[id] = nil
        end
        return id
    end

    function java_ref_methods:invoke(method, sig, ...)
        return java.invoke(self, method, sig, ...)
    end

    java_ref_mt = {
        __index = java_ref_methods,
        __gc = function(self)
            local st = state_of(self)
            if st ~= nil and st.owned and st.id ~= nil then
                release_state(st, false)
            end
        end,
        __close = function(self)
            local st = state_of(self)
            if st ~= nil and st.owned and st.id ~= nil then
                release_state(st, false)
            end
        end,
        __tostring = function(self)
            local st = state_of(self)
            if st ~= nil and st.id ~= nil then
                return tostring(st.id)
            end
            return "java.handle(released)"
        end,
        __concat = function(left, right)
            local left_st = state_of(left)
            if left_st ~= nil then
                assert(left_st.id ~= nil, "Java object handle has already been released")
                left = tostring(left_st.id)
            end
            local right_st = state_of(right)
            if right_st ~= nil then
                assert(right_st.id ~= nil, "Java object handle has already been released")
                right = tostring(right_st.id)
            end
            return left .. right
        end,
        __eq = function(left, right)
            local left_st = state_of(left)
            local right_st = state_of(right)
            if left_st ~= nil and right_st ~= nil then
                return left_st.id ~= nil and left_st.id == right_st.id
            end
            return false
        end,
    }

    local function wrap_handle(raw_id)
        if type(raw_id) ~= "number" or math.type(raw_id) ~= "integer" or raw_id <= 0 then
            return raw_id
        end
        local ref = setmetatable({ [HANDLE_KEY] = { id = raw_id, owned = true } }, java_ref_mt)
        live_refs[raw_id] = ref
        local active = scope_stack[#scope_stack]
        if active ~= nil then
            active._refs[#active._refs + 1] = ref
        end
        return ref
    end

    function java.is_ref(value)
        return state_of(value) ~= nil
    end

    function java.own(raw_id)
        if state_of(raw_id) ~= nil then
            return raw_id
        end
        assert(type(raw_id) == "number" and math.type(raw_id) == "integer" and raw_id > 0,
               "java.own expects a positive integer handle")
        local existing = live_refs[raw_id]
        if existing ~= nil then
            return existing
        end
        return wrap_handle(raw_id)
    end

    function java.release(handle)
        local st = state_of(handle)
        if st ~= nil then
            return release_state(st, true)
        end
        if type(handle) == "number" and math.type(handle) == "integer" and handle > 0 then
            local existing = live_refs[handle]
            if existing ~= nil then
                local existing_st = state_of(existing)
                if existing_st ~= nil then
                    existing_st.id = nil
                    existing_st.owned = false
                end
                live_refs[handle] = nil
            end
        end
        return raw_release(handle)
    end

    if type(raw_invoke) == "function" then
        function java.invoke(target, method, sig, ...)
            local n = select("#", ...)
            local res
            if n == 0 then
                res = raw_invoke(unwrap_arg(target), method, sig)
            else
                local args = table.pack(...)
                for i = 1, n do
                    args[i] = unwrap_arg(args[i])
                end
                res = raw_invoke(unwrap_arg(target), method, sig, table.unpack(args, 1, n))
            end
            if returns_java_object(sig) then
                return wrap_handle(res)
            end
            return res
        end
    end

    if type(raw_new) == "function" then
        function java.new(class_name, ctor_sig, ...)
            local n = select("#", ...)
            local res
            if n == 0 then
                res = raw_new(class_name, ctor_sig)
            else
                local args = table.pack(...)
                for i = 1, n do
                    args[i] = unwrap_arg(args[i])
                end
                res = raw_new(class_name, ctor_sig, table.unpack(args, 1, n))
            end
            return wrap_handle(res)
        end
    end

    if type(raw_load_dex) == "function" then
        function java.load_dex(path)
            return wrap_handle(raw_load_dex(path))
        end
    end

    function java.scope(fn)
        assert(type(fn) == "function", "java.scope expects a function")
        local scope = { _refs = {}, _kept = {} }
        function scope:keep(ref)
            if state_of(ref) ~= nil then
                self._kept[ref] = true
                local parent = scope_stack[#scope_stack - 1]
                if parent ~= nil then
                    parent._refs[#parent._refs + 1] = ref
                end
            end
            return ref
        end
        function scope:track(ref)
            if state_of(ref) ~= nil then
                self._kept[ref] = nil
                self._refs[#self._refs + 1] = ref
            end
            return ref
        end
        function scope:close()
            for i = #self._refs, 1, -1 do
                local ref = self._refs[i]
                self._refs[i] = nil
                if ref ~= nil and not self._kept[ref] then
                    local st = state_of(ref)
                    if st ~= nil and st.owned and st.id ~= nil then
                        release_state(st, false)
                    end
                end
            end
            return true
        end

        scope_stack[#scope_stack + 1] = scope
        local results = table.pack(pcall(fn, scope))
        scope_stack[#scope_stack] = nil
        scope:close()
        if not results[1] then
            error(results[2], 0)
        end
        return table.unpack(results, 2, results.n)
    end

    rawset(java, "_managed_refs", true)
end

local function install_resource_scope()
    local target = rawget(_G, "dejavu")
    if type(target) ~= "table" then
        target = {}
        rawset(_G, "dejavu", target)
    end
    if type(rawget(target, "scope")) == "function" then
        return
    end

    local scope_methods = {}
    local scope_mt = {
        __index = scope_methods,
        __close = function(self)
            self:close()
        end,
    }

    function scope_methods:defer(fn)
        assert(type(fn) == "function", "scope:defer expects a function")
        self._items[#self._items + 1] = fn
        return fn
    end

    function scope_methods:track_rpc(service)
        assert(type(service) == "table" and type(service.unregister) == "function",
               "scope:track_rpc expects an RPC service")
        self:defer(function()
            return service:unregister()
        end)
        return service
    end

    function scope_methods:track_hook(id)
        assert(type(id) == "number", "scope:track_hook expects a hook id")
        self:defer(function()
            if type(hook) == "table" and type(hook.remove) == "function" then
                hook.remove(id)
            end
            return true
        end)
        return id
    end

    function scope_methods:track_native_hook(id)
        assert(type(id) == "number", "scope:track_native_hook expects a native hook id")
        self:defer(function()
            if type(native) == "table" and type(native.uninstall) == "function" then
                native.uninstall(id)
            end
            return true
        end)
        return id
    end

    function scope_methods:track_stream(lease)
        assert(lease ~= nil, "scope:track_stream expects a stream lease")
        self:defer(function()
            if type(hook) == "table" and type(hook.stream_release) == "function" then
                hook.stream_release(lease)
            end
            return true
        end)
        return lease
    end

    function scope_methods:track_java(ref)
        assert(ref ~= nil, "scope:track_java expects a Java object handle")
        self:defer(function()
            if type(java) == "table" and type(java.release) == "function" then
                if java.is_ref and java.is_ref(ref) then
                    if ref:is_alive() then
                        ref:release()
                    end
                else
                    java.release(ref)
                end
            end
            return true
        end)
        return ref
    end

    function scope_methods:count()
        return #self._items
    end

    function scope_methods:close()
        local remaining = {}
        local first_error = nil
        for i = #self._items, 1, -1 do
            local fn = self._items[i]
            local ok, res, detail = pcall(fn)
            if not ok then
                remaining[#remaining + 1] = fn
                if first_error == nil then
                    first_error = tostring(res)
                end
            elseif res == false then
                remaining[#remaining + 1] = fn
                if first_error == nil then
                    first_error = detail ~= nil and tostring(detail) or "cleanup returned false"
                end
            end
        end
        -- Restore failed/busy items in original registration order for safe retry.
        self._items = {}
        for i = #remaining, 1, -1 do
            self._items[#self._items + 1] = remaining[i]
        end
        if first_error ~= nil then
            return false, first_error
        end
        return true
    end

    function target.scope()
        return setmetatable({ _items = {} }, scope_mt)
    end
end

install_java_resource_guards()
install_resource_scope()
