-- Embedded by the agent after host namespaces (java, hook, native, rpc) are registered.
-- 1) Wraps java.* JNI GlobalRef handles in GC-managed + <close>-capable proxies with java.scope().
-- 2) Provides Frida-style java.use(), direct Cls(...) / obj:method(...), field get/set, overload
--    resolution, typed primitive/null markers, proxy args (instance + Class<?>), __cast views,
--    arrays (1-based arr[i]/ipairs + 0-based __get/__set + byte[]), java.each(), java.to_lua(),
--    __instanceof / __str / __inspect, java.app(), and method:hook(c_source).
-- 3) Provides dejavu.scope() for deterministic LIFO cleanup of hooks, RPC services, streams, and Java refs.

local HANDLE_KEY = {}
local CLASS_KEY = {}
local METHOD_KEY = {}
local TYPED_KEY = {}
local live_refs = setmetatable({}, { __mode = "v" })
local class_proxy_cache = setmetatable({}, { __mode = "v" })
local reflect_cache = {}
local loader_reflect_cache = setmetatable({}, { __mode = "k" })
local raw_loader_reflect_cache = {}
local scope_stack = {}
local reflect_for_repl = nil

local PRIMITIVE_TO_JNI = {
    void = "V",
    boolean = "Z",
    byte = "B",
    char = "C",
    short = "S",
    int = "I",
    long = "J",
    float = "F",
    double = "D",
}

local JNI_TO_FRIENDLY = {
    V = "void",
    Z = "boolean",
    B = "byte",
    C = "char",
    S = "short",
    I = "int",
    J = "long",
    F = "float",
    D = "double",
}

local BOXED_INT_CLASSES = {
    ["java.lang.Byte"] = true,
    ["java.lang.Short"] = true,
    ["java.lang.Integer"] = true,
    ["java.lang.Long"] = true,
}

local BOXED_FLOAT_CLASSES = {
    ["java.lang.Float"] = true,
    ["java.lang.Double"] = true,
}

local function normalize_type(name)
    assert(type(name) == "string" and name ~= "", "expected Java or JNI type name")
    local prim = PRIMITIVE_TO_JNI[name]
    if prim ~= nil then
        return prim
    end
    if #name == 1 and JNI_TO_FRIENDLY[name] ~= nil then
        return name
    end
    local dims = 0
    local base = name
    while base:sub(-2) == "[]" do
        dims = dims + 1
        base = base:sub(1, -3)
    end
    if dims > 0 then
        return string.rep("[", dims) .. normalize_type(base)
    end
    if base:sub(1, 1) == "[" then
        return (base:gsub("%.", "/"))
    end
    if base:sub(1, 1) == "L" and base:sub(-1) == ";" then
        return (base:gsub("%.", "/"))
    end
    return "L" .. (base:gsub("%.", "/")) .. ";"
end

local function jni_to_friendly(desc)
    local dims = 0
    local rest = desc
    while rest:sub(1, 1) == "[" do
        dims = dims + 1
        rest = rest:sub(2)
    end
    local base = JNI_TO_FRIENDLY[rest]
    if base == nil then
        if rest:sub(1, 1) == "L" and rest:sub(-1) == ";" then
            base = rest:sub(2, -2):gsub("/", ".")
        else
            base = rest
        end
    end
    if dims > 0 then
        return base .. string.rep("[]", dims)
    end
    return base
end

local function jni_to_class_name(desc)
    if desc:sub(1, 1) == "L" and desc:sub(-1) == ";" then
        return (desc:sub(2, -2):gsub("/", "."))
    end
    if desc:sub(1, 1) == "[" then
        return (desc:gsub("/", "."))
    end
    return nil
end

local function runtime_class_to_jni(cname)
    if cname:sub(1, 1) == "[" then
        return (cname:gsub("%.", "/"))
    end
    return "L" .. (cname:gsub("%.", "/")) .. ";"
end

local function normalize_class_name_for_lookup(name)
    assert(type(name) == "string" and name ~= "", "expected Java class name")
    if name:sub(-2) == "[]" then
        return (normalize_type(name):gsub("/", "."))
    end
    if name:sub(1, 1) == "[" then
        return (name:gsub("/", "."))
    end
    return name
end

local function bytes_to_hex(str)
    return (str:gsub(".", function(ch)
        return string.format("%02x", ch:byte())
    end))
end

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

local function descriptor_is_object(desc)
    if type(desc) ~= "string" or desc == "" or desc == "Ljava/lang/String;" then
        return false
    end
    local prefix = desc:sub(1, 1)
    return prefix == "L" or prefix == "["
end

local function format_overload_params(params)
    local parts = {}
    for i = 1, #params do
        parts[i] = '"' .. jni_to_friendly(params[i]) .. '"'
    end
    return table.concat(parts, ", ")
end

local function format_friendly_params(params)
    local parts = {}
    for i = 1, #params do
        parts[i] = jni_to_friendly(params[i])
    end
    return table.concat(parts, ", ")
end

local function install_java_resource_guards()
    if type(java) ~= "table" or type(java.release) ~= "function" then
        return
    end
    if rawget(java, "_managed_refs") then
        return
    end

    local raw_invoke = java.invoke
    local raw_invoke_as = java._invoke_as
    local raw_new = java.new
    local raw_load_dex = java.load_dex
    local raw_release = java.release
    local raw_reflect = java._reflect
    local raw_class_name = java._class_name
    local raw_class_object = java._class_object
    local raw_is_instance = java._is_instance
    local raw_get_field = java._get_field
    local raw_set_field = java._set_field
    local raw_array_new = java._array_new
    local raw_array_length = java._array_length
    local raw_array_get = java._array_get
    local raw_array_set = java._array_set
    local raw_array_bytes = java._array_bytes

    local java_ref_mt
    local java_class_mt
    local java_method_mt
    local java_overload_mt

    local function state_of(value)
        if type(value) ~= "table" then
            return nil
        end
        return rawget(value, HANDLE_KEY)
    end

    local function root_owner_of(value)
        local cur = value
        local st = state_of(cur)
        if st == nil then
            return nil
        end
        while st ~= nil and st.owner ~= nil do
            cur = st.owner
            st = state_of(cur)
        end
        return cur
    end

    local function root_state_of(value)
        local owner = root_owner_of(value)
        if owner == nil then
            return nil
        end
        return state_of(owner)
    end

    local function class_state_of(value)
        if type(value) ~= "table" then
            return nil
        end
        return rawget(value, CLASS_KEY)
    end

    local function typed_state_of(value)
        if type(value) ~= "table" then
            return nil
        end
        return rawget(value, TYPED_KEY)
    end

    local function make_typed(jni, value)
        return { [TYPED_KEY] = { jni = jni, value = value } }
    end

    local function format_class_target(class_name, loader)
        if loader == nil then
            return class_name
        end
        local lid
        local st = root_state_of(loader)
        if st ~= nil then
            lid = assert(st.id, "ClassLoader handle has already been released")
        else
            assert(type(loader) == "number" and math.type(loader) == "integer" and loader > 0,
                   "loader must be a ClassLoader handle")
            lid = loader
        end
        return tostring(lid) .. ":" .. class_name
    end

    local function release_state(st, propagate_error)
        local id = st.id
        if id == nil then
            return true
        end
        st.id = nil
        st.owned = false
        if live_refs[id] ~= nil and root_state_of(live_refs[id]) == st then
            live_refs[id] = nil
        end
        if propagate_error then
            return raw_release(id)
        end
        pcall(raw_release, id)
        return true
    end

    local function make_unscoped_ref(raw_id, known_class, known_loader)
        if type(raw_id) ~= "number" or math.type(raw_id) ~= "integer" or raw_id <= 0 then
            return raw_id
        end
        local st = {
            id = raw_id,
            owned = true,
            class_name = known_class,
            loader = known_loader,
        }
        local ref = setmetatable({ [HANDLE_KEY] = st }, java_ref_mt)
        live_refs[raw_id] = ref
        return ref
    end

    local function wrap_handle(raw_id, known_class, known_loader)
        local ref = make_unscoped_ref(raw_id, known_class, known_loader)
        if state_of(ref) ~= nil then
            local active = scope_stack[#scope_stack]
            if active ~= nil then
                active._refs[#active._refs + 1] = ref
            end
        end
        return ref
    end

    local function get_class_object_ref(cls_st)
        assert(type(raw_class_object) == "function", "java._class_object is unavailable")
        if cls_st.class_obj ~= nil then
            local obj_st = root_state_of(cls_st.class_obj)
            if obj_st ~= nil and obj_st.id ~= nil then
                return cls_st.class_obj
            end
        end
        local target = format_class_target(cls_st.name, cls_st.loader)
        -- Class object cached on the class proxy outlives any temporary java.scope().
        local ref = make_unscoped_ref(raw_class_object(target), "java.lang.Class", nil)
        cls_st.class_obj = ref
        return ref
    end

    local function unwrap_arg(value)
        local typed = typed_state_of(value)
        if typed ~= nil then
            value = typed.value
            if value == nil then
                return nil
            end
        end
        local st = root_state_of(value)
        if st ~= nil then
            assert(st.id ~= nil, "Java object handle has already been released")
            return st.id
        end
        local cls_st = class_state_of(value)
        if cls_st ~= nil then
            local cls_ref = get_class_object_ref(cls_st)
            return root_state_of(cls_ref).id
        end
        return value
    end

    local function get_runtime_class_name(ref)
        local root_st = root_state_of(ref)
        assert(root_st ~= nil and root_st.id ~= nil, "Java object handle has already been released")
        if root_st.class_name == nil then
            assert(type(raw_class_name) == "function", "java._class_name is unavailable")
            root_st.class_name = raw_class_name(root_st.id)
        end
        return root_st.class_name, root_st.loader
    end

    local function get_ref_class_name(ref)
        local st = assert(state_of(ref), "expected a Java object handle")
        if st.cast_class ~= nil then
            return st.cast_class, st.cast_loader
        end
        return get_runtime_class_name(ref)
    end

    local function get_reflection_meta(class_name, loader, fallback_handle)
        local target = format_class_target(class_name, loader)
        local bucket
        local bucket_key
        if loader == nil then
            bucket = reflect_cache
            bucket_key = class_name
        elseif state_of(loader) ~= nil then
            local owner = root_owner_of(loader)
            bucket = loader_reflect_cache[owner]
            if bucket == nil then
                bucket = {}
                loader_reflect_cache[owner] = bucket
            end
            bucket_key = class_name
        else
            bucket = raw_loader_reflect_cache
            bucket_key = target
        end
        local cached = bucket[bucket_key]
        if cached ~= nil then
            return cached
        end
        assert(type(raw_reflect) == "function", "java._reflect is unavailable")
        assert(type(json) == "table" and type(json.decode) == "function",
               "json.decode is required for Java proxy reflection")
        local ok, raw_json = pcall(raw_reflect, target)
        if ok then
            local meta = json.decode(raw_json)
            bucket[bucket_key] = meta
            return meta
        end
        if fallback_handle ~= nil then
            -- Fallback by concrete object handle (e.g. class from a third-party ClassLoader).
            -- Do not cache under target key to avoid poisoning global reflection metadata.
            local fallback_json = raw_reflect(fallback_handle)
            return json.decode(fallback_json)
        end
        error(raw_json, 0)
    end

    local function array_element_jni(ref)
        local cname = get_runtime_class_name(ref)
        assert(type(cname) == "string" and cname:sub(1, 1) == "[",
               "Java object is not an array: " .. tostring(cname))
        return (cname:sub(2):gsub("%.", "/"))
    end

    local function check_instanceof(ref, class_or_name, loader)
        local root_st = root_state_of(ref)
        if root_st == nil or root_st.id == nil then
            return false
        end
        local target_name
        local target_loader = loader
        local cls_st = class_state_of(class_or_name)
        if cls_st ~= nil then
            target_name = cls_st.name
            target_loader = target_loader or cls_st.loader
        else
            assert(type(class_or_name) == "string" and class_or_name ~= "",
                   "instanceof expects a class proxy or class name string")
            target_name = normalize_class_name_for_lookup(class_or_name)
        end
        local cname = get_runtime_class_name(ref)
        if cname == target_name or target_name == "java.lang.Object" then
            return true
        end
        assert(type(raw_is_instance) == "function", "java._is_instance is unavailable")
        local ok, matches = pcall(raw_is_instance, root_st.id,
                                  format_class_target(target_name, target_loader))
        if not ok and target_loader ~= nil then
            ok, matches = pcall(raw_is_instance, root_st.id, target_name)
        end
        return ok and matches == true
    end

    local function format_inspect(class_name, meta, filter)
        local needle = type(filter) == "string" and filter ~= "" and filter:lower() or nil
        local function matches(text)
            return needle == nil or text:lower():find(needle, 1, true) ~= nil
        end
        local lines = { "class " .. class_name }
        local ctors = meta.constructors or {}
        for i = 1, #ctors do
            local line = "  ctor " .. class_name .. "(" .. format_friendly_params(ctors[i].params or {}) .. ")"
            if matches(line) then
                lines[#lines + 1] = line
            end
        end
        local field_names = {}
        for name in pairs(meta.fields or {}) do
            field_names[#field_names + 1] = name
        end
        table.sort(field_names)
        for i = 1, #field_names do
            local name = field_names[i]
            local f = meta.fields[name]
            local line = "  field " .. (f.static and "static " or "") ..
                         jni_to_friendly(f.sig) .. " " .. name
            if matches(line) then
                lines[#lines + 1] = line
            end
        end
        local method_names = {}
        for name in pairs(meta.methods or {}) do
            method_names[#method_names + 1] = name
        end
        table.sort(method_names)
        for i = 1, #method_names do
            local name = method_names[i]
            local overloads = meta.methods[name]
            for j = 1, #overloads do
                local ov = overloads[j]
                local line = "  method " .. (ov.static and "static " or "") ..
                             jni_to_friendly(ov.ret or "V") .. " " .. name .. "(" ..
                             format_friendly_params(ov.params or {}) .. ")"
                if matches(line) then
                    lines[#lines + 1] = line
                end
            end
        end
        return table.concat(lines, "\n")
    end

    local function score_arg(value, param_jni, loader)
        local typed = typed_state_of(value)
        if typed ~= nil then
            return typed.jni == param_jni and 6 or nil
        end
        local tv = type(value)
        if value == nil then
            if param_jni:sub(1, 1) == "L" or param_jni:sub(1, 1) == "[" then
                return 1
            end
            return nil
        end
        if tv == "boolean" then
            return param_jni == "Z" and 4 or nil
        end
        if tv == "string" then
            if param_jni == "Ljava/lang/String;" then
                return 4
            elseif param_jni == "Ljava/lang/CharSequence;" then
                return 3
            elseif param_jni == "Ljava/lang/Object;" then
                return 2
            end
            return nil
        end
        if tv == "number" then
            if math.type(value) == "integer" then
                local in_i32 = value >= -2147483648 and value <= 2147483647
                if param_jni == "I" then
                    return in_i32 and 4 or nil
                elseif param_jni == "J" then
                    return in_i32 and 3 or 4
                elseif param_jni == "S" then
                    return (value >= -32768 and value <= 32767) and 2 or nil
                elseif param_jni == "B" then
                    return (value >= -128 and value <= 127) and 2 or nil
                elseif param_jni == "C" then
                    return (value >= 0 and value <= 65535) and 2 or nil
                elseif param_jni == "D" then
                    return 1.5
                elseif param_jni == "F" then
                    return 1
                end
                return nil
            else
                if param_jni == "D" then
                    return 4
                elseif param_jni == "F" then
                    return 3
                end
                return nil
            end
        end
        local cls_st = class_state_of(value)
        if cls_st ~= nil then
            if param_jni == "Ljava/lang/Class;" then
                return 5
            elseif param_jni == "Ljava/lang/Object;" then
                return 2
            end
            return nil
        end
        local root_st = root_state_of(value)
        if root_st ~= nil then
            assert(root_st.id ~= nil, "Java object handle has already been released")
            if param_jni:sub(1, 1) ~= "L" and param_jni:sub(1, 1) ~= "[" then
                return nil
            end
            local cname = get_ref_class_name(value)
            local arg_jni = runtime_class_to_jni(cname)
            if arg_jni == param_jni then
                return 5
            end
            if param_jni == "Ljava/lang/Object;" then
                return 2
            end
            if type(raw_is_instance) == "function" then
                local target_cls = jni_to_class_name(param_jni)
                if target_cls ~= nil then
                    local ok, matches = pcall(raw_is_instance, root_st.id,
                                              format_class_target(target_cls, loader))
                    if not ok and loader ~= nil then
                        ok, matches = pcall(raw_is_instance, root_st.id, target_cls)
                    end
                    if ok and matches then
                        return 3
                    end
                end
            end
            return nil
        end
        return nil
    end

    local function select_overload(owner_name, member_name, overloads, args, arg_count, loader)
        local best_score = -1
        local best = {}
        for i = 1, #overloads do
            local ov = overloads[i]
            local params = ov.params or {}
            if #params == arg_count then
                local total = 0
                local ok = true
                for j = 1, arg_count do
                    local s = score_arg(args[j], params[j], loader)
                    if s == nil then
                        ok = false
                        break
                    end
                    total = total + s
                end
                if ok then
                    if total > best_score then
                        best_score = total
                        best = { ov }
                    elseif total == best_score then
                        best[#best + 1] = ov
                    end
                end
            end
        end
        if #best == 1 then
            return best[1]
        end
        local candidates = {}
        local list = #best > 1 and best or overloads
        for i = 1, #list do
            candidates[i] = ":overload(" .. format_overload_params(list[i].params or {}) .. ")"
        end
        if #best > 1 then
            error("ambiguous overload for " .. owner_name .. "." .. member_name ..
                  " (" .. tostring(arg_count) .. " arg(s)); specify " ..
                  table.concat(candidates, " or "), 0)
        end
        error("no matching overload for " .. owner_name .. "." .. member_name ..
              " (" .. tostring(arg_count) .. " arg(s)); available: " ..
              table.concat(candidates, ", "), 0)
    end

    local function filter_overloads(overloads, is_static_only)
        local out = {}
        for i = 1, #overloads do
            if is_static_only then
                if overloads[i].static then
                    out[#out + 1] = overloads[i]
                end
            else
                if not overloads[i].static then
                    out[#out + 1] = overloads[i]
                end
            end
        end
        if #out == 0 and not is_static_only then
            -- Allow calling static methods via an instance reference (Java semantics).
            return overloads
        end
        return out
    end

    local function invoke_selected(m_st, ov, args, arg_count)
        local unwrapped = {}
        for i = 1, arg_count do
            unwrapped[i] = unwrap_arg(args[i])
        end
        if m_st.kind == "ctor" then
            local target = format_class_target(m_st.class_name, m_st.loader)
            local raw_id = raw_new(target, ov.sig, table.unpack(unwrapped, 1, arg_count))
            return wrap_handle(raw_id, m_st.class_name, m_st.loader)
        end
        local res
        if ov.static then
            local target = format_class_target(m_st.class_name, m_st.loader)
            res = raw_invoke(target, m_st.name, ov.sig, table.unpack(unwrapped, 1, arg_count))
        else
            local recv = m_st.bound_self
            if recv == nil then
                error("instance method " .. m_st.class_name .. "." .. m_st.name ..
                      " requires a Java object receiver (call on an instance, or use :hook(src))", 0)
            end
            local root_st = assert(root_state_of(recv), "instance method requires a Java object")
            assert(root_st.id ~= nil, "Java object handle has already been released")
            local st = state_of(recv)
            if st ~= nil and st.cast_class ~= nil and type(raw_invoke_as) == "function" then
                local cast_target = format_class_target(st.cast_class, st.cast_loader)
                res = raw_invoke_as(root_st.id, cast_target, m_st.name, ov.sig,
                                    table.unpack(unwrapped, 1, arg_count))
            else
                res = raw_invoke(root_st.id, m_st.name, ov.sig, table.unpack(unwrapped, 1, arg_count))
            end
        end
        if returns_java_object(ov.sig) then
            return wrap_handle(res, nil, m_st.loader)
        end
        return res
    end

    local function any_overload_matches(overloads, args, arg_count, loader)
        for i = 1, #overloads do
            local params = overloads[i].params or {}
            if #params == arg_count then
                local ok = true
                for j = 1, arg_count do
                    if score_arg(args[j], params[j], loader) == nil then
                        ok = false
                        break
                    end
                end
                if ok then
                    return true
                end
            end
        end
        return false
    end

    local function strip_bound_self(m_st, packed, n, exact_arity)
        if m_st.bound_self ~= nil and n >= 1 and rawequal(packed[1], m_st.bound_self) then
            if exact_arity ~= nil then
                if n == exact_arity + 1 then
                    local shifted = {}
                    for i = 2, n do
                        shifted[i - 1] = packed[i]
                    end
                    return shifted, n - 1
                end
                return packed, n
            end
            local shifted = {}
            for i = 2, n do
                shifted[i - 1] = packed[i]
            end
            -- Prefer colon-call stripping whenever any overload type-matches the shifted arguments.
            if any_overload_matches(m_st.overloads, shifted, n - 1, m_st.loader) then
                return shifted, n - 1
            end
            -- If unstripped arguments also do not match, strip only if an overload has arity n-1
            -- so error messages reflect the colon-call arity.
            if not any_overload_matches(m_st.overloads, packed, n, m_st.loader) then
                for i = 1, #m_st.overloads do
                    if #(m_st.overloads[i].params or {}) == n - 1 then
                        return shifted, n - 1
                    end
                end
            end
        end
        return packed, n
    end

    local function install_method_hook(m_st, sig, source)
        if m_st.kind == "ctor" then
            error("method:hook does not support constructors (<init>); LSPlant only hooks Java methods", 0)
        end
        if m_st.loader ~= nil then
            error("method:hook only supports classes in the app ClassLoader (custom DexClassLoader hooks are unsupported)", 0)
        end
        assert(type(hook) == "table" and type(hook.install) == "function",
               "hook.install is unavailable")
        assert(type(source) == "string" and source ~= "", "method:hook expects C hook source")
        return hook.install(m_st.class_name, m_st.name, sig, source)
    end

    local overload_proxy_helpers = {}

    function overload_proxy_helpers:hook(source)
        local ov_st = assert(rawget(self, METHOD_KEY), "expected a Java overload proxy")
        return install_method_hook(ov_st.m_st, ov_st.ov.sig, source)
    end

    function overload_proxy_helpers:signature()
        local ov_st = assert(rawget(self, METHOD_KEY), "expected a Java overload proxy")
        return ov_st.ov.sig
    end

    java_overload_mt = {
        __index = function(self, key)
            if key == "sig" then
                return rawget(self, METHOD_KEY).ov.sig
            end
            return overload_proxy_helpers[key]
        end,
        __call = function(self, ...)
            local ov_st = rawget(self, METHOD_KEY)
            local packed = table.pack(...)
            local args, arg_count = strip_bound_self(ov_st.m_st, packed, packed.n, ov_st.arity)
            return invoke_selected(ov_st.m_st, ov_st.ov, args, arg_count)
        end,
        __tostring = function(self)
            local ov_st = rawget(self, METHOD_KEY)
            return "java.overload(" .. ov_st.m_st.class_name .. "." .. ov_st.m_st.name .. ov_st.ov.sig .. ")"
        end,
    }

    local method_proxy_helpers = {}

    function method_proxy_helpers:overload(...)
        local m_st = assert(rawget(self, METHOD_KEY), "expected a Java method proxy")
        local n = select("#", ...)
        local wanted = {}
        for i = 1, n do
            wanted[i] = normalize_type(select(i, ...))
        end
        local matched = nil
        for i = 1, #m_st.overloads do
            local ov = m_st.overloads[i]
            local params = ov.params or {}
            if #params == n then
                local same = true
                for j = 1, n do
                    if params[j] ~= wanted[j] then
                        same = false
                        break
                    end
                end
                if same then
                    matched = ov
                    break
                end
            end
        end
        if matched == nil then
            local available = {}
            for i = 1, #m_st.overloads do
                available[i] = "(" .. format_overload_params(m_st.overloads[i].params or {}) .. ")"
            end
            error("overload (" .. format_overload_params(wanted) .. ") not found on " ..
                  m_st.class_name .. "." .. m_st.name .. "; available: " ..
                  table.concat(available, ", "), 0)
        end
        return setmetatable({
            [METHOD_KEY] = { m_st = m_st, ov = matched, arity = n },
        }, java_overload_mt)
    end

    function method_proxy_helpers:overloads()
        local m_st = assert(rawget(self, METHOD_KEY), "expected a Java method proxy")
        local out = {}
        for i = 1, #m_st.overloads do
            out[i] = m_st.overloads[i].sig
        end
        return out
    end

    function method_proxy_helpers:hook(source)
        local m_st = assert(rawget(self, METHOD_KEY), "expected a Java method proxy")
        if #m_st.overloads ~= 1 then
            local candidates = {}
            for i = 1, #m_st.overloads do
                candidates[i] = ":overload(" .. format_overload_params(m_st.overloads[i].params or {}) .. ")"
            end
            error("method " .. m_st.class_name .. "." .. m_st.name .. " has " ..
                  tostring(#m_st.overloads) .. " overloads; select one with " ..
                  table.concat(candidates, " or "), 0)
        end
        return install_method_hook(m_st, m_st.overloads[1].sig, source)
    end

    java_method_mt = {
        __index = method_proxy_helpers,
        __call = function(self, ...)
            local m_st = rawget(self, METHOD_KEY)
            local packed = table.pack(...)
            local args, arg_count = strip_bound_self(m_st, packed, packed.n, nil)
            local ov = select_overload(m_st.class_name, m_st.name, m_st.overloads,
                                       args, arg_count, m_st.loader)
            return invoke_selected(m_st, ov, args, arg_count)
        end,
        __tostring = function(self)
            local m_st = rawget(self, METHOD_KEY)
            return "java.method(" .. m_st.class_name .. "." .. m_st.name .. ")"
        end,
    }

    local function make_method_proxy(kind, class_name, loader, name, overloads, bound_self)
        return setmetatable({
            [METHOD_KEY] = {
                kind = kind,
                class_name = class_name,
                loader = loader,
                name = name,
                overloads = overloads,
                bound_self = bound_self,
            },
        }, java_method_mt)
    end

    local function read_field_value(target, field_name, field_sig, declaring_class, loader)
        assert(type(raw_get_field) == "function", "java._get_field is unavailable")
        local res
        if declaring_class ~= nil then
            res = raw_get_field(target, field_name, field_sig, declaring_class)
        else
            res = raw_get_field(target, field_name, field_sig)
        end
        if descriptor_is_object(field_sig) then
            return wrap_handle(res, nil, loader)
        end
        return res
    end

    local function write_field_value(target, field_name, field_sig, value, declaring_class)
        assert(type(raw_set_field) == "function", "java._set_field is unavailable")
        local unwrapped = unwrap_arg(value)
        if declaring_class ~= nil then
            return raw_set_field(target, field_name, field_sig, unwrapped, declaring_class)
        end
        return raw_set_field(target, field_name, field_sig, unwrapped)
    end

    local ref_meta_methods = {}

    ref_meta_methods["__id"] = function(self)
        local st = root_state_of(self)
        return st and st.id or nil
    end

    ref_meta_methods["__is_alive"] = function(self)
        local st = root_state_of(self)
        return st ~= nil and st.id ~= nil
    end

    ref_meta_methods["__release"] = function(self)
        local st = assert(root_state_of(self), "expected a Java object handle")
        return release_state(st, true)
    end

    ref_meta_methods["__detach"] = function(self)
        local st = assert(root_state_of(self), "expected a Java object handle")
        local id = assert(st.id, "Java object handle has already been released")
        st.id = nil
        st.owned = false
        if live_refs[id] ~= nil and root_state_of(live_refs[id]) == st then
            live_refs[id] = nil
        end
        return id
    end

    ref_meta_methods["__class_name"] = function(self)
        local cname = get_ref_class_name(self)
        return cname
    end

    ref_meta_methods["__class"] = function(self)
        local cname, loader = get_ref_class_name(self)
        return java.use(cname, loader)
    end

    ref_meta_methods["__instanceof"] = function(self, class_or_name, loader)
        return check_instanceof(self, class_or_name, loader)
    end

    ref_meta_methods["__str"] = function(self)
        local root_st = assert(root_state_of(self), "expected a Java object handle")
        assert(root_st.id ~= nil, "Java object handle has already been released")
        return raw_invoke(root_st.id, "toString", "()Ljava/lang/String;")
    end

    ref_meta_methods["__inspect"] = function(self, filter)
        local root_st = assert(root_state_of(self), "expected a Java object handle")
        assert(root_st.id ~= nil, "Java object handle has already been released")
        local st = state_of(self)
        local cname, loader = get_ref_class_name(self)
        local fallback = (st ~= nil and st.cast_class ~= nil) and nil or root_st.id
        local meta = get_reflection_meta(cname, loader, fallback)
        return format_inspect(cname, meta, filter)
    end

    ref_meta_methods["__each"] = function(self)
        return java.each(self)
    end

    ref_meta_methods["__cast"] = function(self, class_or_name, loader)
        local root_st = assert(root_state_of(self), "expected a Java object handle")
        assert(root_st.id ~= nil, "Java object handle has already been released")
        local target_name
        local target_loader = loader
        local cls_st = class_state_of(class_or_name)
        if cls_st ~= nil then
            target_name = cls_st.name
            target_loader = target_loader or cls_st.loader
        else
            assert(type(class_or_name) == "string" and class_or_name ~= "",
                   "__cast expects a class proxy or class name")
            target_name = normalize_class_name_for_lookup(class_or_name)
        end
        target_loader = target_loader or root_st.loader
        assert(check_instanceof(self, target_name, target_loader),
               "__cast: " .. tostring(get_runtime_class_name(self)) ..
               " is not an instance of " .. target_name)
        local root_owner = root_owner_of(self)
        local view_st = {
            owner = root_owner,
            cast_class = target_name,
            cast_loader = target_loader,
        }
        return setmetatable({ [HANDLE_KEY] = view_st }, java_ref_mt)
    end

    ref_meta_methods["__invoke"] = function(self, method, sig, ...)
        return java.invoke(self, method, sig, ...)
    end

    ref_meta_methods["__field"] = function(self, field_name)
        assert(type(field_name) == "string" and field_name ~= "", "__field expects a field name")
        local root_st = assert(root_state_of(self), "expected a Java object handle")
        assert(root_st.id ~= nil, "Java object handle has already been released")
        local st = state_of(self)
        local cname, loader = get_ref_class_name(self)
        local fallback = (st ~= nil and st.cast_class ~= nil) and nil or root_st.id
        local meta = get_reflection_meta(cname, loader, fallback)
        local field = meta.fields and meta.fields[field_name]
        assert(field ~= nil, "field not found: " .. cname .. "." .. field_name)
        if field.static then
            return read_field_value(format_class_target(cname, loader), field_name, field.sig, nil, loader)
        end
        local decl = st and st.cast_class and format_class_target(st.cast_class, st.cast_loader) or nil
        return read_field_value(root_st.id, field_name, field.sig, decl, loader)
    end

    ref_meta_methods["__set_field"] = function(self, field_name, value)
        assert(type(field_name) == "string" and field_name ~= "", "__set_field expects a field name")
        local root_st = assert(root_state_of(self), "expected a Java object handle")
        assert(root_st.id ~= nil, "Java object handle has already been released")
        local st = state_of(self)
        local cname, loader = get_ref_class_name(self)
        local fallback = (st ~= nil and st.cast_class ~= nil) and nil or root_st.id
        local meta = get_reflection_meta(cname, loader, fallback)
        local field = meta.fields and meta.fields[field_name]
        assert(field ~= nil, "field not found: " .. cname .. "." .. field_name)
        if field.static then
            return write_field_value(format_class_target(cname, loader), field_name, field.sig, value, nil)
        end
        local decl = st and st.cast_class and format_class_target(st.cast_class, st.cast_loader) or nil
        return write_field_value(root_st.id, field_name, field.sig, value, decl)
    end

    ref_meta_methods["__length"] = function(self)
        local root_st = assert(root_state_of(self), "expected a Java array handle")
        assert(root_st.id ~= nil, "Java object handle has already been released")
        array_element_jni(self)
        assert(type(raw_array_length) == "function", "java._array_length is unavailable")
        return raw_array_length(root_st.id)
    end

    ref_meta_methods["__get"] = function(self, index)
        local root_st = assert(root_state_of(self), "expected a Java array handle")
        assert(root_st.id ~= nil, "Java object handle has already been released")
        local elem_jni = array_element_jni(self)
        assert(type(raw_array_get) == "function", "java._array_get is unavailable")
        local val = raw_array_get(root_st.id, elem_jni, index)
        if descriptor_is_object(elem_jni) then
            return wrap_handle(val, nil, root_st.loader)
        end
        return val
    end

    ref_meta_methods["__set"] = function(self, index, value)
        local root_st = assert(root_state_of(self), "expected a Java array handle")
        assert(root_st.id ~= nil, "Java object handle has already been released")
        local elem_jni = array_element_jni(self)
        assert(type(raw_array_set) == "function", "java._array_set is unavailable")
        return raw_array_set(root_st.id, elem_jni, index, unwrap_arg(value))
    end

    ref_meta_methods["__bytes"] = function(self)
        local root_st = assert(root_state_of(self), "expected a Java byte[] handle")
        assert(root_st.id ~= nil, "Java object handle has already been released")
        local elem_jni = array_element_jni(self)
        assert(elem_jni == "B", "__bytes requires a byte[] array, got [" .. elem_jni)
        assert(type(raw_array_bytes) == "function", "java._array_bytes is unavailable")
        return raw_array_bytes(root_st.id)
    end

    ref_meta_methods["__to_table"] = function(self)
        local len = ref_meta_methods["__length"](self)
        local out = {}
        for i = 0, len - 1 do
            out[i + 1] = ref_meta_methods["__get"](self, i)
        end
        return out
    end

    java_ref_mt = {
        __index = function(self, key)
            local meta_fn = ref_meta_methods[key]
            if meta_fn ~= nil then
                return meta_fn
            end
            -- 1-based Lua array indexing (supports arr[i] and built-in ipairs(arr)).
            if type(key) == "number" and math.type(key) == "integer" then
                local len = ref_meta_methods["__length"](self)
                if key < 1 or key > len then
                    return nil
                end
                return ref_meta_methods["__get"](self, key - 1)
            end
            if type(key) ~= "string" then
                return nil
            end
            local root_st = root_state_of(self)
            assert(root_st ~= nil and root_st.id ~= nil, "Java object handle has already been released")
            local st = state_of(self)
            local cname, loader = get_ref_class_name(self)
            local fallback = (st ~= nil and st.cast_class ~= nil) and nil or root_st.id
            local meta = get_reflection_meta(cname, loader, fallback)
            local overloads = meta.methods and meta.methods[key]
            if overloads ~= nil and #overloads > 0 then
                return make_method_proxy("method", cname, loader, key,
                                         filter_overloads(overloads, false), self)
            end
            local field = meta.fields and meta.fields[key]
            if field ~= nil then
                if field.static then
                    return read_field_value(format_class_target(cname, loader), key, field.sig, nil, loader)
                end
                local decl = st and st.cast_class and format_class_target(st.cast_class, st.cast_loader) or nil
                return read_field_value(root_st.id, key, field.sig, decl, loader)
            end
            error("member not found on " .. cname .. ": " .. key, 2)
        end,
        __newindex = function(self, key, value)
            if type(key) == "number" and math.type(key) == "integer" then
                ref_meta_methods["__set"](self, key - 1, value)
                return
            end
            assert(type(key) == "string" and key ~= "", "Java field name must be a string")
            ref_meta_methods["__set_field"](self, key, value)
        end,
        __len = function(self)
            return ref_meta_methods["__length"](self)
        end,
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
            local st = root_state_of(self)
            if st ~= nil and st.id ~= nil then
                return tostring(st.id)
            end
            return "java.handle(released)"
        end,
        __concat = function(left, right)
            local left_st = root_state_of(left)
            if left_st ~= nil then
                assert(left_st.id ~= nil, "Java object handle has already been released")
                left = tostring(left_st.id)
            end
            local right_st = root_state_of(right)
            if right_st ~= nil then
                assert(right_st.id ~= nil, "Java object handle has already been released")
                right = tostring(right_st.id)
            end
            return left .. right
        end,
        __eq = function(left, right)
            local left_st = root_state_of(left)
            local right_st = root_state_of(right)
            if left_st ~= nil and right_st ~= nil then
                return left_st.id ~= nil and left_st.id == right_st.id
            end
            return false
        end,
    }

    java_class_mt = {
        __index = function(self, key)
            local cls_st = rawget(self, CLASS_KEY)
            if key == "__name" then
                return cls_st.name
            elseif key == "__loader" then
                return cls_st.loader
            elseif key == "__class" then
                return get_class_object_ref(cls_st)
            elseif key == "__methods" then
                return get_reflection_meta(cls_st.name, cls_st.loader, nil).methods
            elseif key == "__fields" then
                return get_reflection_meta(cls_st.name, cls_st.loader, nil).fields
            elseif key == "__inspect" then
                return function(_, filter)
                    if type(_) == "string" and filter == nil then
                        filter = _
                    end
                    local meta = get_reflection_meta(cls_st.name, cls_st.loader, nil)
                    return format_inspect(cls_st.name, meta, filter)
                end
            elseif key == "__new" then
                local meta = get_reflection_meta(cls_st.name, cls_st.loader, nil)
                local ctors = meta.constructors or {}
                assert(#ctors > 0, "no constructors found on " .. cls_st.name)
                return make_method_proxy("ctor", cls_st.name, cls_st.loader, "<init>", ctors, self)
            elseif key == "__field" then
                return function(_, field_name)
                    if type(_) == "string" and field_name == nil then
                        field_name = _
                    end
                    local meta = get_reflection_meta(cls_st.name, cls_st.loader, nil)
                    local field = meta.fields and meta.fields[field_name]
                    assert(field ~= nil and field.static,
                           "static field not found: " .. cls_st.name .. "." .. tostring(field_name))
                    return read_field_value(format_class_target(cls_st.name, cls_st.loader),
                                            field_name, field.sig, nil, cls_st.loader)
                end
            elseif key == "__set_field" then
                return function(_, field_name, value)
                    if type(_) == "string" and value == nil then
                        value = field_name
                        field_name = _
                    end
                    local meta = get_reflection_meta(cls_st.name, cls_st.loader, nil)
                    local field = meta.fields and meta.fields[field_name]
                    assert(field ~= nil and field.static,
                           "static field not found: " .. cls_st.name .. "." .. tostring(field_name))
                    return write_field_value(format_class_target(cls_st.name, cls_st.loader),
                                             field_name, field.sig, value, nil)
                end
            end
            if type(key) ~= "string" then
                return nil
            end
            local meta = get_reflection_meta(cls_st.name, cls_st.loader, nil)
            local overloads = meta.methods and meta.methods[key]
            if overloads ~= nil and #overloads > 0 then
                local statics = filter_overloads(overloads, true)
                if #statics > 0 then
                    return make_method_proxy("method", cls_st.name, cls_st.loader, key, statics, self)
                end
            end
            if key == "new" then
                local ctors = meta.constructors or {}
                if #ctors > 0 then
                    return make_method_proxy("ctor", cls_st.name, cls_st.loader, "<init>", ctors, self)
                end
            end
            local field = meta.fields and meta.fields[key]
            if field ~= nil and field.static then
                return read_field_value(format_class_target(cls_st.name, cls_st.loader),
                                        key, field.sig, nil, cls_st.loader)
            end
            if overloads ~= nil and #overloads > 0 then
                -- Return unbound instance method proxy for :hook() / :overload() inspection.
                return make_method_proxy("method", cls_st.name, cls_st.loader, key, overloads, nil)
            end
            error("member not found on " .. cls_st.name .. ": " .. key, 2)
        end,
        __call = function(self, ...)
            local cls_st = rawget(self, CLASS_KEY)
            local meta = get_reflection_meta(cls_st.name, cls_st.loader, nil)
            local ctors = meta.constructors or {}
            assert(#ctors > 0, "no constructors found on " .. cls_st.name)
            local ctor_proxy = make_method_proxy("ctor", cls_st.name, cls_st.loader, "<init>", ctors, nil)
            return ctor_proxy(...)
        end,
        __newindex = function(self, key, value)
            local cls_st = rawget(self, CLASS_KEY)
            assert(type(key) == "string" and key ~= "", "Java static field name must be a string")
            local meta = get_reflection_meta(cls_st.name, cls_st.loader, nil)
            local field = meta.fields and meta.fields[key]
            assert(field ~= nil and field.static,
                   "static field not found: " .. cls_st.name .. "." .. key)
            write_field_value(format_class_target(cls_st.name, cls_st.loader), key, field.sig, value, nil)
        end,
        __tostring = function(self)
            local cls_st = rawget(self, CLASS_KEY)
            return "java.class(" .. cls_st.name .. ")"
        end,
    }

    function java.use(class_name, loader)
        assert(type(class_name) == "string" and class_name ~= "", "java.use expects a class name")
        if loader == nil then
            local lid_str, rest = class_name:match("^(%d+):(.+)$")
            if lid_str ~= nil and rest ~= "" then
                local lid = tonumber(lid_str)
                if lid ~= nil and math.type(lid) == "integer" and lid > 0 then
                    loader = live_refs[lid] or lid
                    class_name = rest
                end
            end
        end
        local key = format_class_target(class_name, loader)
        local existing = class_proxy_cache[key]
        if existing ~= nil then
            local existing_st = class_state_of(existing)
            if existing_st ~= nil then
                local existing_loader = existing_st.loader
                local loader_ok = true
                if existing_loader ~= nil and state_of(existing_loader) ~= nil then
                    loader_ok = java.is_alive(existing_loader)
                end
                if loader_ok and (loader == nil or existing_loader == loader or
                                  (state_of(existing_loader) ~= nil and state_of(loader) == nil)) then
                    return existing
                end
            end
        end
        -- Eagerly reflect so invalid class names fail immediately on java.use().
        get_reflection_meta(class_name, loader, nil)
        local proxy = setmetatable({
            [CLASS_KEY] = {
                name = class_name,
                loader = loader,
                class_obj = nil,
            },
        }, java_class_mt)
        class_proxy_cache[key] = proxy
        return proxy
    end

    function java.app()
        return java.use("android.app.ActivityThread").currentApplication()
    end

    function java.cast(ref, class_name, loader)
        assert(state_of(ref) ~= nil, "java.cast expects a Java object handle")
        return ref_meta_methods["__cast"](ref, class_name, loader)
    end

    function java.instanceof(ref, class_or_name, loader)
        return check_instanceof(ref, class_or_name, loader)
    end

    function java.str(ref)
        if state_of(ref) ~= nil then
            return ref_meta_methods["__str"](ref)
        end
        return tostring(ref)
    end

    function java.boolean(v)
        assert(type(v) == "boolean", "java.boolean expects a boolean")
        return make_typed("Z", v)
    end
    function java.byte(v)
        assert(type(v) == "number" and math.type(v) == "integer", "java.byte expects an integer")
        return make_typed("B", v)
    end
    function java.char(v)
        if type(v) == "string" and #v == 1 then
            v = v:byte(1)
        end
        assert(type(v) == "number" and math.type(v) == "integer", "java.char expects an integer or 1-byte char")
        return make_typed("C", v)
    end
    function java.short(v)
        assert(type(v) == "number" and math.type(v) == "integer", "java.short expects an integer")
        return make_typed("S", v)
    end
    function java.int(v)
        assert(type(v) == "number" and math.type(v) == "integer", "java.int expects an integer")
        return make_typed("I", v)
    end
    function java.long(v)
        assert(type(v) == "number" and math.type(v) == "integer", "java.long expects an integer")
        return make_typed("J", v)
    end
    function java.float(v)
        assert(type(v) == "number", "java.float expects a number")
        return make_typed("F", v)
    end
    function java.double(v)
        assert(type(v) == "number", "java.double expects a number")
        return make_typed("D", v)
    end
    function java.null(type_name)
        local jni = normalize_type(type_name)
        assert(jni:sub(1, 1) == "L" or jni:sub(1, 1) == "[",
               "java.null expects a Java reference or array type")
        return make_typed(jni, nil)
    end

    java.NULL = setmetatable({}, {
        __tostring = function()
            return "java.NULL"
        end,
    })

    function java.each(obj)
        assert(state_of(obj) ~= nil, "java.each expects a Java array, Iterable, or Map")
        local cname = get_runtime_class_name(obj)
        if type(cname) == "string" and cname:sub(1, 1) == "[" then
            local len = ref_meta_methods["__length"](obj)
            local idx = 0
            local function iter()
                if idx >= len then
                    return nil
                end
                local cur = idx
                idx = idx + 1
                return idx, ref_meta_methods["__get"](obj, cur)
            end
            return iter, nil, nil
        end
        local function make_closer(refs)
            return setmetatable({}, {
                __close = function()
                    for i = #refs, 1, -1 do
                        local r = refs[i]
                        if r ~= nil and java.is_alive(r) then
                            pcall(java.release, r)
                        end
                    end
                end,
            })
        end
        if check_instanceof(obj, "java.util.Map") then
            local entry_set = wrap_handle(raw_invoke(root_state_of(obj).id, "entrySet", "()Ljava/util/Set;"))
            local ok_it, it_or_err = pcall(function()
                return wrap_handle(raw_invoke(root_state_of(entry_set).id, "iterator", "()Ljava/util/Iterator;"))
            end)
            if not ok_it then
                if java.is_alive(entry_set) then
                    pcall(java.release, entry_set)
                end
                error(it_or_err, 0)
            end
            local it = it_or_err
            local closer = make_closer({ entry_set, it })
            local function iter()
                if not java.is_alive(it) or not raw_invoke(root_state_of(it).id, "hasNext", "()Z") then
                    getmetatable(closer).__close()
                    return nil
                end
                local raw_entry = raw_invoke(root_state_of(it).id, "next", "()Ljava/lang/Object;")
                if raw_entry == nil then
                    return java.NULL, nil
                end
                local entry <close> = make_unscoped_ref(raw_entry)
                local eid = root_state_of(entry).id
                local k = wrap_handle(raw_invoke(eid, "getKey", "()Ljava/lang/Object;"))
                local v = wrap_handle(raw_invoke(eid, "getValue", "()Ljava/lang/Object;"))
                return (k == nil and java.NULL or k), v
            end
            return iter, nil, nil, closer
        end
        if check_instanceof(obj, "java.lang.Iterable") then
            local it = wrap_handle(raw_invoke(root_state_of(obj).id, "iterator", "()Ljava/util/Iterator;"))
            local closer = make_closer({ it })
            local idx = 0
            local function iter()
                if not java.is_alive(it) or not raw_invoke(root_state_of(it).id, "hasNext", "()Z") then
                    getmetatable(closer).__close()
                    return nil
                end
                idx = idx + 1
                local item = wrap_handle(raw_invoke(root_state_of(it).id, "next", "()Ljava/lang/Object;"))
                return idx, item
            end
            return iter, nil, nil, closer
        end
        error("java.each requires a Java array, Iterable, or Map; got " .. tostring(cname), 2)
    end

    function java.to_lua(value, opts)
        opts = opts or {}
        local max_depth = opts.max_depth or 8
        local max_items = opts.max_items or 256
        local bytes_mode = opts.bytes or "raw"
        assert(bytes_mode == "raw" or bytes_mode == "hex",
               "java.to_lua opts.bytes must be 'raw' or 'hex'")
        local null_sentinel = type(json) == "table" and json.null or nil
        local function make_array()
            if type(json) == "table" and type(json.array) == "function" then
                return json.array()
            end
            return {}
        end
        local function convert(val, depth)
            if val == nil or type(val) ~= "table" or state_of(val) == nil then
                return val
            end
            local root_st = root_state_of(val)
            assert(root_st ~= nil and root_st.id ~= nil, "Java object handle has already been released")
            local id = root_st.id
            local cname = get_runtime_class_name(val)
            if cname == "java.lang.String" then
                return raw_invoke(id, "toString", "()Ljava/lang/String;")
            elseif cname == "java.lang.Boolean" then
                return raw_invoke(id, "booleanValue", "()Z")
            elseif BOXED_INT_CLASSES[cname] then
                return raw_invoke(id, "longValue", "()J")
            elseif BOXED_FLOAT_CLASSES[cname] then
                return raw_invoke(id, "doubleValue", "()D")
            elseif cname == "java.lang.Character" then
                return raw_invoke(id, "toString", "()Ljava/lang/String;")
            elseif cname == "[B" then
                local raw = ref_meta_methods["__bytes"](val)
                return bytes_mode == "hex" and bytes_to_hex(raw) or raw
            elseif depth >= max_depth then
                return raw_invoke(id, "toString", "()Ljava/lang/String;")
            elseif type(cname) == "string" and cname:sub(1, 1) == "[" then
                local arr = make_array()
                local len = ref_meta_methods["__length"](val)
                local limit = len < max_items and len or max_items
                local elem_jni = array_element_jni(val)
                local is_obj = descriptor_is_object(elem_jni)
                for i = 0, limit - 1 do
                    local raw_elem = raw_array_get(id, elem_jni, i)
                    local converted
                    if raw_elem == nil then
                        converted = null_sentinel
                    elseif is_obj then
                        local elem <close> = make_unscoped_ref(raw_elem, nil, root_st.loader)
                        converted = convert(elem, depth + 1)
                        if converted == nil then
                            converted = null_sentinel
                        end
                    else
                        converted = raw_elem
                    end
                    arr[i + 1] = converted
                end
                return arr
            elseif check_instanceof(val, "java.util.Map") then
                local out = {}
                local count = 0
                local entry_set <close> = make_unscoped_ref(raw_invoke(id, "entrySet", "()Ljava/util/Set;"))
                local it <close> = make_unscoped_ref(
                    raw_invoke(root_state_of(entry_set).id, "iterator", "()Ljava/util/Iterator;"))
                local it_id = root_state_of(it).id
                while count < max_items and raw_invoke(it_id, "hasNext", "()Z") do
                    count = count + 1
                    local raw_entry = raw_invoke(it_id, "next", "()Ljava/lang/Object;")
                    if raw_entry ~= nil then
                        local entry <close> = make_unscoped_ref(raw_entry)
                        local eid = root_state_of(entry).id
                        local raw_k = raw_invoke(eid, "getKey", "()Ljava/lang/Object;")
                        local raw_v = raw_invoke(eid, "getValue", "()Ljava/lang/Object;")
                        local lk
                        if raw_k == nil then
                            lk = "null"
                        else
                            local k <close> = make_unscoped_ref(raw_k)
                            lk = tostring(convert(k, depth + 1))
                        end
                        local lv
                        if raw_v == nil then
                            lv = null_sentinel
                        else
                            local v <close> = make_unscoped_ref(raw_v)
                            lv = convert(v, depth + 1)
                            if lv == nil then
                                lv = null_sentinel
                            end
                        end
                        out[lk] = lv
                    end
                end
                return out
            elseif check_instanceof(val, "java.lang.Iterable") then
                local arr = make_array()
                local count = 0
                local it <close> = make_unscoped_ref(raw_invoke(id, "iterator", "()Ljava/util/Iterator;"))
                local it_id = root_state_of(it).id
                while count < max_items and raw_invoke(it_id, "hasNext", "()Z") do
                    count = count + 1
                    local raw_item = raw_invoke(it_id, "next", "()Ljava/lang/Object;")
                    local converted
                    if raw_item == nil then
                        converted = null_sentinel
                    else
                        local item <close> = make_unscoped_ref(raw_item)
                        converted = convert(item, depth + 1)
                        if converted == nil then
                            converted = null_sentinel
                        end
                    end
                    arr[count] = converted
                end
                return arr
            elseif check_instanceof(val, "java.lang.CharSequence") then
                return raw_invoke(id, "toString", "()Ljava/lang/String;")
            end
            return raw_invoke(id, "toString", "()Ljava/lang/String;")
        end
        return convert(value, 0)
    end

    function java.is_ref(value)
        return state_of(value) ~= nil
    end

    function java.is_class(value)
        return class_state_of(value) ~= nil
    end

    function java.id(value)
        local st = root_state_of(value)
        if st ~= nil then
            return st.id
        end
        if type(value) == "number" and math.type(value) == "integer" and value > 0 then
            return value
        end
        return nil
    end

    function java.is_alive(value)
        local st = root_state_of(value)
        return st ~= nil and st.id ~= nil
    end

    function java.detach(value)
        assert(state_of(value) ~= nil, "java.detach expects a Java object handle")
        return ref_meta_methods["__detach"](value)
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
        return wrap_handle(raw_id, nil, nil)
    end

    function java.bytes(str)
        assert(type(str) == "string", "java.bytes expects a Lua string")
        assert(type(raw_array_new) == "function", "java._array_new is unavailable")
        return wrap_handle(raw_array_new("B", str), "[B", nil)
    end

    function java.array(elem_type, len_or_values, loader)
        assert(type(raw_array_new) == "function", "java._array_new is unavailable")
        local elem_jni = normalize_type(elem_type)
        local lid = loader ~= nil and unwrap_arg(loader) or nil
        if elem_jni == "B" and type(len_or_values) == "string" then
            return wrap_handle(raw_array_new("B", len_or_values), "[B", loader)
        end
        if type(len_or_values) == "number" then
            local raw_id = lid ~= nil and raw_array_new(elem_jni, len_or_values, lid)
                or raw_array_new(elem_jni, len_or_values)
            return wrap_handle(raw_id, "[" .. (elem_jni:gsub("/", ".")), loader)
        end
        assert(type(len_or_values) == "table",
               "java.array expects a length, byte string, or table of elements")
        local len = #len_or_values
        local raw_id = lid ~= nil and raw_array_new(elem_jni, len, lid)
            or raw_array_new(elem_jni, len)
        local arr = wrap_handle(raw_id, "[" .. (elem_jni:gsub("/", ".")), loader)
        for i = 1, len do
            raw_array_set(raw_id, elem_jni, i - 1, unwrap_arg(len_or_values[i]))
        end
        return arr
    end

    function java.release(handle)
        local st = root_state_of(handle)
        if st ~= nil then
            return release_state(st, true)
        end
        if type(handle) == "number" and math.type(handle) == "integer" and handle > 0 then
            local existing = live_refs[handle]
            if existing ~= nil then
                local existing_st = root_state_of(existing)
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
            local args
            if n > 0 then
                args = table.pack(...)
                for i = 1, n do
                    args[i] = unwrap_arg(args[i])
                end
            end
            local res
            local cls_st = class_state_of(target)
            if cls_st ~= nil then
                local cls_target = format_class_target(cls_st.name, cls_st.loader)
                if n == 0 then
                    res = raw_invoke(cls_target, method, sig)
                else
                    res = raw_invoke(cls_target, method, sig, table.unpack(args, 1, n))
                end
            else
                local st = state_of(target)
                if st ~= nil and st.cast_class ~= nil and type(raw_invoke_as) == "function" then
                    local root_st = root_state_of(target)
                    assert(root_st ~= nil and root_st.id ~= nil,
                           "Java object handle has already been released")
                    local cast_target = format_class_target(st.cast_class, st.cast_loader)
                    if n == 0 then
                        res = raw_invoke_as(root_st.id, cast_target, method, sig)
                    else
                        res = raw_invoke_as(root_st.id, cast_target, method, sig,
                                            table.unpack(args, 1, n))
                    end
                else
                    local raw_target = unwrap_arg(target)
                    if n == 0 then
                        res = raw_invoke(raw_target, method, sig)
                    else
                        res = raw_invoke(raw_target, method, sig, table.unpack(args, 1, n))
                    end
                end
            end
            if returns_java_object(sig) then
                return wrap_handle(res, nil, nil)
            end
            return res
        end
    end

    if type(raw_new) == "function" then
        function java.new(class_name, ctor_sig, ...)
            local cls_st = class_state_of(class_name)
            local target = cls_st ~= nil and format_class_target(cls_st.name, cls_st.loader) or class_name
            local n = select("#", ...)
            local res
            if n == 0 then
                res = raw_new(target, ctor_sig)
            else
                local args = table.pack(...)
                for i = 1, n do
                    args[i] = unwrap_arg(args[i])
                end
                res = raw_new(target, ctor_sig, table.unpack(args, 1, n))
            end
            return wrap_handle(res, cls_st and cls_st.name or nil, cls_st and cls_st.loader or nil)
        end
    end

    if type(raw_load_dex) == "function" then
        function java.load_dex(path)
            return wrap_handle(raw_load_dex(path), "dalvik.system.InMemoryDexClassLoader", nil)
        end
    end

    function java.scope(fn)
        assert(type(fn) == "function", "java.scope expects a function")
        local parent = scope_stack[#scope_stack]
        local scope = { _refs = {}, _kept = {} }
        function scope:keep(ref)
            local owner = root_owner_of(ref)
            if owner ~= nil then
                self._kept[owner] = true
                if parent ~= nil then
                    parent._refs[#parent._refs + 1] = owner
                end
            end
            return ref
        end
        function scope:track(ref)
            local owner = root_owner_of(ref)
            if owner ~= nil then
                self._kept[owner] = nil
                self._refs[#self._refs + 1] = owner
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

    reflect_for_repl = function(val)
        local cls_st = class_state_of(val)
        if cls_st ~= nil then
            local ok, meta = pcall(get_reflection_meta, cls_st.name, cls_st.loader, nil)
            return ok and meta or nil, "class", cls_st.name
        end
        local root_st = root_state_of(val)
        if root_st ~= nil then
            if root_st.id == nil then
                return nil, "released", nil
            end
            local st = state_of(val)
            local ok_name, cname, loader = pcall(get_ref_class_name, val)
            if not ok_name or type(cname) ~= "string" then
                return nil, "ref", nil
            end
            local fallback = (st ~= nil and st.cast_class ~= nil) and nil or root_st.id
            local ok_meta, meta = pcall(get_reflection_meta, cname, loader, fallback)
            return ok_meta and meta or nil, "ref", cname
        end
        return nil, nil, nil
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
        local is_valid_ref = type(java) == "table" and type(java.is_ref) == "function" and java.is_ref(ref)
        local is_valid_int = type(ref) == "number" and math.type(ref) == "integer" and ref > 0
        assert(is_valid_ref or is_valid_int,
               "scope:track_java expects a Java object handle or positive handle id")
        self:defer(function()
            if type(java) == "table" and type(java.release) == "function" then
                if java.is_ref and java.is_ref(ref) then
                    if java.is_alive(ref) then
                        java.release(ref)
                    end
                else
                    local ok, err = pcall(java.release, ref)
                    if not ok and not tostring(err):find("unknown Java object handle", 1, true) then
                        return false, err
                    end
                end
            end
            return true
        end)
        return ref
    end

    function scope_methods:track_sqlite(db)
        local sq = rawget(_G, "sqlite")
        local is_valid_db = type(sq) == "table" and type(sq.is_db) == "function" and sq.is_db(db)
        assert(is_valid_db, "scope:track_sqlite expects a SQLite database handle")
        self:defer(function()
            if db:is_open() then
                db:close()
            end
            return true
        end)
        return db
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

local function install_repl_runtime()
    -- Pin stdlib functions as upvalues so shadowing globals like `table`, `string`,
    -- `type`, or `rawget` in the REPL never bricks _repl_eval or leaks repl_print.
    local type = type
    local rawget = rawget
    local rawset = rawset
    local rawlen = rawlen
    local next = next
    local pairs = pairs
    local pcall = pcall
    local xpcall = xpcall
    local tostring = tostring
    local select = select
    local load = load
    local getmetatable = getmetatable
    local setmetatable = setmetatable
    local collectgarbage = collectgarbage
    local t_concat = table.concat
    local t_sort = table.sort
    local t_pack = table.pack
    local s_format = string.format
    local s_rep = string.rep
    local m_type = math.type
    local m_min = math.min
    local m_huge = math.huge
    local debug_lib = rawget(_G, "debug")
    local debug_traceback = type(debug_lib) == "table" and rawget(debug_lib, "traceback") or nil

    local target = rawget(_G, "dejavu")
    if type(target) ~= "table" then
        target = {}
        rawset(_G, "dejavu", target)
    end
    if type(rawget(target, "_repl_eval")) == "function" then
        return
    end

    local LUA_KEYWORDS = {
        "and", "break", "do", "else", "elseif", "end", "false", "for",
        "function", "goto", "if", "in", "local", "nil", "not", "or",
        "repeat", "return", "then", "true", "until", "while",
    }

    local REF_META_COMPLETIONS = {
        "__bytes", "__cast", "__class", "__class_name", "__detach",
        "__each", "__field", "__get", "__id", "__inspect", "__instanceof",
        "__invoke", "__is_alive", "__length", "__release", "__set",
        "__set_field", "__str", "__to_table",
    }

    local CLASS_META_DOT_COMPLETIONS = {
        "__class", "__field", "__fields", "__inspect", "__loader",
        "__methods", "__name", "__new", "__set_field",
    }

    local CLASS_META_COLON_COMPLETIONS = {
        "__field", "__inspect", "__new", "__set_field",
    }

    local JSON_ARRAY_MT = {}
    local function json_array(list)
        return setmetatable(list, JSON_ARRAY_MT)
    end

    local function utf8_seq_len(s, i)
        local b1 = s:byte(i)
        if b1 == nil then
            return 0
        end
        if b1 < 0x80 then
            return 1
        end
        local b2 = s:byte(i + 1)
        if b2 == nil or b2 < 0x80 or b2 > 0xbf then
            return 0
        end
        if b1 >= 0xc2 and b1 <= 0xdf then
            return 2
        end
        local b3 = s:byte(i + 2)
        if b3 == nil or b3 < 0x80 or b3 > 0xbf then
            return 0
        end
        if b1 >= 0xe0 and b1 <= 0xef then
            if b1 == 0xe0 and b2 < 0xa0 then return 0 end
            if b1 == 0xed and b2 > 0x9f then return 0 end
            return 3
        end
        local b4 = s:byte(i + 3)
        if b4 == nil or b4 < 0x80 or b4 > 0xbf then
            return 0
        end
        if b1 >= 0xf0 and b1 <= 0xf4 then
            if b1 == 0xf0 and b2 < 0x90 then return 0 end
            if b1 == 0xf4 and b2 > 0x8f then return 0 end
            return 4
        end
        return 0
    end

    local function truncate_utf8(s, max_bytes)
        if #s <= max_bytes then
            return s, false
        end
        local i = 1
        local last_end = 0
        while i <= max_bytes do
            local seq = utf8_seq_len(s, i)
            local step = seq > 0 and seq or 1
            if i + step - 1 > max_bytes then
                break
            end
            last_end = i + step - 1
            i = i + step
        end
        return s:sub(1, last_end), true
    end

    local function safe_tostring(val)
        local ok, s = pcall(tostring, val)
        if ok and type(s) == "string" then
            return s
        end
        return "<" .. type(val) .. ">"
    end

    local function json_escape_string(s)
        local out = {}
        local n = #s
        local i = 1
        while i <= n do
            local b = s:byte(i)
            if b == 0x5c then
                out[#out + 1] = "\\\\"
                i = i + 1
            elseif b == 0x22 then
                out[#out + 1] = '\\"'
                i = i + 1
            elseif b == 0x08 then
                out[#out + 1] = "\\b"
                i = i + 1
            elseif b == 0x0c then
                out[#out + 1] = "\\f"
                i = i + 1
            elseif b == 0x0a then
                out[#out + 1] = "\\n"
                i = i + 1
            elseif b == 0x0d then
                out[#out + 1] = "\\r"
                i = i + 1
            elseif b == 0x09 then
                out[#out + 1] = "\\t"
                i = i + 1
            elseif b < 0x20 or b == 0x7f then
                out[#out + 1] = s_format("\\u%04x", b)
                i = i + 1
            elseif b < 0x80 then
                out[#out + 1] = s:sub(i, i)
                i = i + 1
            else
                local seq = utf8_seq_len(s, i)
                if seq > 0 then
                    out[#out + 1] = s:sub(i, i + seq - 1)
                    i = i + seq
                else
                    out[#out + 1] = s_format("\\u%04x", b)
                    i = i + 1
                end
            end
        end
        return t_concat(out)
    end

    local function json_encode(val)
        local tv = type(val)
        if val == nil then
            return "null"
        elseif tv == "boolean" then
            return val and "true" or "false"
        elseif tv == "number" then
            if val ~= val or val == m_huge or val == -m_huge then
                return "null"
            end
            if m_type(val) == "integer" then
                return s_format("%d", val)
            end
            -- Normalize comma decimal separators under non-C LC_NUMERIC locales.
            return (tostring(val):gsub(",", "."))
        elseif tv == "string" then
            return '"' .. json_escape_string(val) .. '"'
        elseif tv == "table" then
            if getmetatable(val) == JSON_ARRAY_MT then
                local items = {}
                for i = 1, rawlen(val) do
                    items[i] = json_encode(rawget(val, i))
                end
                return "[" .. t_concat(items, ",") .. "]"
            end
            local keys = {}
            for k in next, val do
                if type(k) == "string" then
                    keys[#keys + 1] = k
                end
            end
            t_sort(keys)
            local items = {}
            for i = 1, #keys do
                local k = keys[i]
                items[i] = '"' .. json_escape_string(k) .. '":' .. json_encode(rawget(val, k))
            end
            return "{" .. t_concat(items, ",") .. "}"
        end
        return '"' .. json_escape_string(safe_tostring(val)) .. '"'
    end

    local function escape_display_string(s, max_len)
        local total_len = #s
        local limit = (max_len ~= nil and total_len > max_len) and max_len or total_len
        local truncated = limit < total_len
        local out = {}
        local i = 1
        while i <= limit do
            local b = s:byte(i)
            if b == 0x5c then
                out[#out + 1] = "\\\\"
                i = i + 1
            elseif b == 0x22 then
                out[#out + 1] = '\\"'
                i = i + 1
            elseif b == 0x0a then
                out[#out + 1] = "\\n"
                i = i + 1
            elseif b == 0x0d then
                out[#out + 1] = "\\r"
                i = i + 1
            elseif b == 0x09 then
                out[#out + 1] = "\\t"
                i = i + 1
            elseif b == 0x00 then
                out[#out + 1] = "\\0"
                i = i + 1
            elseif b < 0x20 or b == 0x7f then
                out[#out + 1] = s_format("\\x%02x", b)
                i = i + 1
            elseif b < 0x80 then
                out[#out + 1] = s:sub(i, i)
                i = i + 1
            else
                local seq = utf8_seq_len(s, i)
                if seq > 0 then
                    if i + seq - 1 > limit then
                        truncated = true
                        break
                    end
                    out[#out + 1] = s:sub(i, i + seq - 1)
                    i = i + seq
                else
                    out[#out + 1] = s_format("\\x%02x", b)
                    i = i + 1
                end
            end
        end
        local escaped = t_concat(out)
        if truncated then
            return '"' .. escaped .. '..." (' .. tostring(total_len) .. " bytes)"
        end
        return '"' .. escaped .. '"'
    end

    local function format_overload_sig(ov)
        local params = ov.params or {}
        local parts = {}
        for i = 1, #params do
            parts[i] = jni_to_friendly(params[i])
        end
        local ret = ov.ret and jni_to_friendly(ov.ret) or "void"
        return "(" .. t_concat(parts, ", ") .. ") -> " .. ret
    end

    local function format_java_ref(v)
        local alive_ok, alive = pcall(v.__is_alive, v)
        if not alive_ok or not alive then
            return "<java.ref (released)>"
        end
        local id_ok, id = pcall(v.__id, v)
        local id_tag = (id_ok and id ~= nil) and (" #" .. tostring(id)) or ""
        local cls_ok, cname = pcall(v.__class_name, v)
        if not cls_ok or type(cname) ~= "string" or cname == "" then
            return "<java.ref" .. id_tag .. ">"
        end
        if cname == "[B" then
            local len_ok, len = pcall(v.__length, v)
            local len_str = (len_ok and type(len) == "number") and tostring(len) or "?"
            local preview = ""
            if len_ok and type(len) == "number" and len > 0 and len <= 16 then
                local b_ok, raw = pcall(v.__bytes, v)
                if b_ok and type(raw) == "string" then
                    local hex = {}
                    for i = 1, #raw do
                        hex[i] = s_format("%02x", raw:byte(i))
                    end
                    preview = " [" .. t_concat(hex, " ") .. "]"
                end
            end
            return "<byte[" .. len_str .. "]" .. id_tag .. preview .. ">"
        end
        if cname:sub(1, 1) == "[" then
            local friendly = jni_to_friendly(runtime_class_to_jni(cname))
            local len_ok, len = pcall(v.__length, v)
            if len_ok and type(len) == "number" then
                local sized = friendly:gsub("%[%]", "[" .. tostring(len) .. "]", 1)
                return "<" .. sized .. id_tag .. ">"
            end
            return "<" .. friendly .. id_tag .. ">"
        end
        local str_ok, str_val = pcall(v.__str, v)
        if str_ok and type(str_val) == "string" then
            return "<" .. cname .. id_tag .. " " .. escape_display_string(str_val, 80) .. ">"
        end
        return "<" .. cname .. id_tag .. ">"
    end

    local function is_identifier_key(k)
        return type(k) == "string" and #k <= 128 and k:match("^[A-Za-z_][A-Za-z0-9_]*$") ~= nil
    end

    local function key_entry_cmp(a, b)
        local ta, tb = a.tk, b.tk
        if ta == tb then
            if ta == "number" or ta == "string" then
                return a.k < b.k
            end
            return a.s < b.s
        end
        if ta == "number" then return true end
        if tb == "number" then return false end
        if ta == "string" then return true end
        if tb == "string" then return false end
        return ta < tb
    end

    local function consume_budget(budget, rendered)
        budget.bytes = budget.bytes - #rendered
        return rendered
    end

    local function format_value(v, depth, seen, max_depth, max_items, top_str_limit, budget)
        local tv = type(v)
        if v == nil then
            return consume_budget(budget, "nil")
        elseif tv == "boolean" then
            return consume_budget(budget, v and "true" or "false")
        elseif tv == "number" then
            return consume_budget(budget, tostring(v))
        elseif tv == "string" then
            local limit = depth == 0 and (top_str_limit or 4096) or 512
            return consume_budget(budget, escape_display_string(v, limit))
        elseif tv ~= "table" then
            return consume_budget(budget, safe_tostring(v))
        end

        -- Check Dejavu Java proxy types via module-private marker keys.
        local cls_st = rawget(v, CLASS_KEY)
        if cls_st ~= nil then
            return consume_budget(budget, "<java.class " .. tostring(cls_st.name) .. ">")
        end
        local handle_st = rawget(v, HANDLE_KEY)
        if handle_st ~= nil then
            return consume_budget(budget, format_java_ref(v))
        end
        local method_st = rawget(v, METHOD_KEY)
        if method_st ~= nil then
            if method_st.ov ~= nil and method_st.m_st ~= nil then
                return consume_budget(
                    budget,
                    "<java.overload " .. method_st.m_st.class_name .. "." ..
                    method_st.m_st.name .. format_overload_sig(method_st.ov) .. ">"
                )
            end
            local ovs = method_st.overloads or {}
            local shown = {}
            local cap = m_min(#ovs, 3)
            for i = 1, cap do
                shown[i] = format_overload_sig(ovs[i])
            end
            local suffix = #ovs > cap and (" | +" .. tostring(#ovs - cap) .. " more") or ""
            return consume_budget(
                budget,
                "<java.method " .. method_st.class_name .. "." .. method_st.name ..
                t_concat(shown, " | ") .. suffix .. ">"
            )
        end
        local typed_st = rawget(v, TYPED_KEY)
        if typed_st ~= nil then
            local friendly = typed_st.jni and jni_to_friendly(typed_st.jni) or "unknown"
            if typed_st.value == nil then
                return consume_budget(budget, "<java.null " .. friendly .. ">")
            end
            return consume_budget(
                budget,
                "<java.typed " .. friendly .. " = " .. safe_tostring(typed_st.value) .. ">"
            )
        end

        local mt = getmetatable(v)
        if type(mt) == "table" then
            local ts = rawget(mt, "__tostring")
            if type(ts) == "function" then
                local ok, s = pcall(ts, v)
                if ok and type(s) == "string" then
                    local clipped, was_clipped = truncate_utf8(s, 4096)
                    if was_clipped then
                        clipped = clipped .. "..."
                    end
                    return consume_budget(budget, clipped)
                end
            end
        end

        if seen[v] then
            return consume_budget(budget, "<cycle>")
        end
        if depth >= max_depth or budget.nodes <= 0 or budget.bytes <= 0 then
            return consume_budget(budget, "{ ... }")
        end

        budget.nodes = budget.nodes - 1
        seen[v] = true

        -- Check whether table is a pure 1..N sequence without triggering __len / __index,
        -- bounding the scan for huge tables.
        local seq_len = rawlen(v)
        local scan_cap = max_items * 16
        local total_keys = 0
        local scan_capped = false
        local is_seq = seq_len > 0
        for k in next, v do
            total_keys = total_keys + 1
            if is_seq then
                if type(k) ~= "number" or m_type(k) ~= "integer" or k < 1 or k > seq_len then
                    is_seq = false
                end
            end
            if total_keys >= scan_cap then
                scan_capped = true
                if is_seq and seq_len > total_keys then
                    -- Keep sequence fast path only when remaining keys are beyond scan_cap.
                    is_seq = false
                end
                break
            end
        end
        if total_keys == 0 then
            seen[v] = nil
            return consume_budget(budget, "{}")
        end
        if is_seq and total_keys ~= seq_len then
            is_seq = false
        end

        local indent = s_rep("  ", depth + 1)
        local close_indent = s_rep("  ", depth)
        local entries = {}
        local multiline = false
        local inline_len = 2

        if is_seq then
            local limit = m_min(seq_len, max_items)
            local rendered_count = 0
            for i = 1, limit do
                if budget.nodes <= 0 or budget.bytes <= 0 then
                    break
                end
                local rendered = format_value(
                    rawget(v, i), depth + 1, seen, max_depth, max_items, top_str_limit, budget
                )
                entries[#entries + 1] = rendered
                rendered_count = rendered_count + 1
                if rendered:find("\n", 1, true) then
                    multiline = true
                end
                inline_len = inline_len + #rendered + 2
                budget.bytes = budget.bytes - (#indent + 2)
            end
            if seq_len > rendered_count then
                entries[#entries + 1] = "... (+" .. tostring(seq_len - rendered_count) .. " more)"
                multiline = true
            end
        else
            local key_entries = {}
            local collected = 0
            local has_more_keys = false
            for k in next, v do
                if collected >= scan_cap then
                    has_more_keys = true
                    break
                end
                collected = collected + 1
                local tk = type(k)
                key_entries[collected] = {
                    k = k,
                    tk = tk,
                    s = (tk == "number" or tk == "string") and "" or safe_tostring(k),
                }
            end
            t_sort(key_entries, key_entry_cmp)
            local limit = m_min(#key_entries, max_items)
            local rendered_count = 0
            for i = 1, limit do
                if budget.nodes <= 0 or budget.bytes <= 0 then
                    break
                end
                local k = key_entries[i].k
                local key_str
                if is_identifier_key(k) then
                    key_str = consume_budget(budget, k)
                else
                    key_str = "[" .. format_value(
                        k, depth + 1, seen, max_depth, max_items, top_str_limit, budget
                    ) .. "]"
                    budget.bytes = budget.bytes - 2
                end
                local val_str
                if budget.bytes <= 0 then
                    val_str = consume_budget(budget, "...")
                else
                    val_str = format_value(
                        rawget(v, k), depth + 1, seen, max_depth, max_items, top_str_limit, budget
                    )
                end
                local entry = key_str .. " = " .. val_str
                entries[#entries + 1] = entry
                rendered_count = rendered_count + 1
                if entry:find("\n", 1, true) then
                    multiline = true
                end
                inline_len = inline_len + #entry + 2
                budget.bytes = budget.bytes - (#indent + 5)
            end
            if has_more_keys or scan_capped then
                entries[#entries + 1] = "... (+" .. tostring(#key_entries - rendered_count) .. "+ more)"
                multiline = true
            elseif #key_entries > rendered_count then
                entries[#entries + 1] = "... (+" .. tostring(#key_entries - rendered_count) .. " more)"
                multiline = true
            end
            if #entries > 4 then
                multiline = true
            end
        end

        seen[v] = nil

        if not multiline and inline_len <= 72 then
            return "{ " .. t_concat(entries, ", ") .. " }"
        end
        local lines = {}
        for i = 1, #entries do
            lines[i] = indent .. entries[i]
        end
        return "{\n" .. t_concat(lines, ",\n") .. "\n" .. close_indent .. "}"
    end

    function target._repl_format(value, opts)
        local max_depth = (type(opts) == "table" and rawget(opts, "max_depth")) or 4
        local max_items = (type(opts) == "table" and rawget(opts, "max_items")) or 64
        local max_str = (type(opts) == "table" and rawget(opts, "max_str")) or 4096
        local max_bytes = (type(opts) == "table" and rawget(opts, "max_bytes")) or 49152
        local budget = {
            nodes = (type(opts) == "table" and rawget(opts, "max_nodes")) or 256,
            bytes = max_bytes,
        }
        local rendered = format_value(value, 0, {}, max_depth, max_items, max_str, budget)
        if #rendered > max_bytes then
            local suffix = "... (truncated)"
            local keep = max_bytes > #suffix and (max_bytes - #suffix) or max_bytes
            local clipped = truncate_utf8(rendered, keep)
            rendered = max_bytes > #suffix and (clipped .. suffix) or clipped
        end
        return rendered
    end

    local function is_incomplete_error(err, code)
        if type(err) ~= "string" then
            return false
        end
        if not err:match("<eof>%s*$") then
            return false
        end
        if err:find("unfinished string near <eof>", 1, true) then
            -- Lua 5.4 allows short string multiline continuations with trailing \ or \z<whitespace>.
            local trailing_bs = code:match("(\\+)$")
            if trailing_bs ~= nil and (#trailing_bs % 2 == 1) then
                return true
            end
            local z_bs = code:match("(\\+)z%s*$")
            return z_bs ~= nil and (#z_bs % 2 == 1)
        end
        return true
    end

    local function clean_error_message(err)
        return safe_tostring(err)
    end

    function target._repl_eval(code)
        if type(code) ~= "string" then
            return json_encode({
                ok = false,
                incomplete = false,
                stdout = "",
                error = "expected string code",
            })
        end
        if code:match("^%s*$") then
            return json_encode({
                ok = true,
                incomplete = false,
                stdout = "",
                values = json_array({}),
            })
        end

        local chunk, expr_err = load("return " .. code, "=repl", "t")
        local stmt_err = nil
        if chunk == nil then
            chunk, stmt_err = load(code, "=repl", "t")
        end

        if chunk == nil then
            if is_incomplete_error(stmt_err, code) or is_incomplete_error(expr_err, code) then
                return json_encode({
                    ok = false,
                    incomplete = true,
                    stdout = "",
                    error = clean_error_message(stmt_err or expr_err),
                })
            end
            return json_encode({
                ok = false,
                incomplete = false,
                stdout = "",
                error = clean_error_message(stmt_err or expr_err),
            })
        end

        local out_lines = {}
        local out_bytes = 0
        local out_truncated = false
        local MAX_STDOUT_LINES = 512
        local MAX_STDOUT_BYTES = 65536

        local orig_print = rawget(_G, "print")
        local function repl_print(...)
            if out_truncated then
                return
            end
            local n = select("#", ...)
            local parts = {}
            for i = 1, n do
                parts[i] = tostring(select(i, ...))
            end
            local line = t_concat(parts, "\t")
            if #out_lines >= MAX_STDOUT_LINES or out_bytes + #line > MAX_STDOUT_BYTES then
                out_truncated = true
                out_lines[#out_lines + 1] = "... (stdout truncated)"
                return
            end
            out_lines[#out_lines + 1] = line
            out_bytes = out_bytes + #line + 1
        end

        local function err_handler(e)
            local msg = safe_tostring(e)
            if type(debug_traceback) == "function" then
                local tb = debug_traceback("", 2)
                if type(tb) == "string" then
                    local lines = {}
                    for line in (tb .. "\n"):gmatch("(.-)\n") do
                        lines[#lines + 1] = line
                    end
                    -- Trim from the bottom at the REPL wrapper frame and its preceding xpcall frame.
                    local cut = #lines
                    for i = #lines, 1, -1 do
                        if lines[i]:find("'_repl_eval'", 1, true) or
                           lines[i]:find("in field '_repl_eval'", 1, true) then
                            cut = i - 1
                            if cut >= 1 and lines[cut]:find("in function 'xpcall'", 1, true) then
                                cut = cut - 1
                            end
                            break
                        end
                    end
                    local kept = {}
                    for i = 1, cut do
                        if lines[i] ~= "" then
                            kept[#kept + 1] = lines[i]
                        end
                    end
                    if #kept > 0 then
                        return msg .. "\n" .. t_concat(kept, "\n")
                    end
                end
            end
            return msg
        end

        rawset(_G, "print", repl_print)
        local packed = t_pack(xpcall(chunk, err_handler))
        if rawget(_G, "print") == repl_print then
            rawset(_G, "print", orig_print)
        end

        local stdout_text = t_concat(out_lines, "\n")
        if not packed[1] then
            return json_encode({
                ok = false,
                incomplete = false,
                stdout = stdout_text,
                error = clean_error_message(packed[2]),
            })
        end

        local values = {}
        local MAX_RETURN_VALUES = 64
        if packed.n >= 2 then
            if packed[2] ~= nil then
                rawset(_G, "_", packed[2])
            end
            local total_vals = packed.n - 1
            local cap = m_min(total_vals, MAX_RETURN_VALUES)
            for i = 2, cap + 1 do
                local ok_fmt, formatted = pcall(target._repl_format, packed[i])
                if ok_fmt and type(formatted) == "string" then
                    values[#values + 1] = formatted
                else
                    values[#values + 1] = "<format error: " .. safe_tostring(formatted) .. ">"
                end
            end
            if total_vals > cap then
                values[#values + 1] = "... (+" .. tostring(total_vals - cap) .. " more values)"
            end
        end

        return json_encode({
            ok = true,
            incomplete = false,
            stdout = stdout_text,
            values = json_array(values),
        })
    end

    local function resolve_readonly_path(expr)
        if type(expr) ~= "string" or expr == "" or expr:find("..", 1, true) then
            return nil, false
        end
        local segments = {}
        for seg in expr:gmatch("[^%.]+") do
            if not seg:match("^[A-Za-z_][A-Za-z0-9_]*$") then
                return nil, false
            end
            segments[#segments + 1] = seg
        end
        if #segments == 0 or expr:sub(1, 1) == "." or expr:sub(-1) == "." then
            return nil, false
        end
        local cur = rawget(_G, segments[1])
        if cur == nil then
            return nil, false
        end
        for i = 2, #segments do
            if type(cur) ~= "table" then
                return nil, false
            end
            -- Allow read-only method proxy metadata lookup on the final segment of Cls.method
            -- (e.g. SB.append:ho<Tab>) without ever reading static fields or invoking JNI calls.
            if rawget(cur, CLASS_KEY) ~= nil then
                if i == #segments and type(reflect_for_repl) == "function" then
                    local meta, kind = reflect_for_repl(cur)
                    if kind == "class" and type(meta) == "table" and type(meta.methods) == "table" then
                        local ovs = meta.methods[segments[i]]
                        if type(ovs) == "table" and #ovs > 0 then
                            return { [METHOD_KEY] = { overloads = ovs } }, true
                        end
                    end
                end
                return nil, false
            end
            -- Never invoke arbitrary __index metamethods during Tab completion.
            if rawget(cur, HANDLE_KEY) ~= nil or
               rawget(cur, METHOD_KEY) ~= nil or rawget(cur, TYPED_KEY) ~= nil then
                return nil, false
            end
            cur = rawget(cur, segments[i])
            if cur == nil then
                return nil, false
            end
        end
        return cur, true
    end

    local function add_if_match(bucket, seen, name, partial, include_dunder)
        if type(name) ~= "string" or name == "" then
            return
        end
        if not include_dunder and name:sub(1, 2) == "__" then
            return
        end
        if partial ~= "" and name:sub(1, #partial) ~= partial then
            return
        end
        if not seen[name] then
            seen[name] = true
            bucket[#bucket + 1] = name
        end
    end

    function target._repl_complete(text)
        if type(text) ~= "string" then
            return "[]"
        end
        local matches = {}
        local seen = {}
        local base, sep, partial = text:match("^(.*)([%.:])([A-Za-z0-9_]*)$")
        if base ~= nil and base ~= "" then
            local val, ok = resolve_readonly_path(base)
            if not ok or (type(val) ~= "table" and type(val) ~= "userdata") then
                return "[]"
            end
            local include_dunder = partial:sub(1, 1) == "_"
            if type(val) == "userdata" then
                local mt = getmetatable(val)
                local idx_tbl = type(mt) == "table" and rawget(mt, "__index") or nil
                if type(idx_tbl) == "table" then
                    for k in next, idx_tbl do
                        if is_identifier_key(k) then
                            add_if_match(matches, seen, k, partial, include_dunder)
                        end
                    end
                end
            else
                local meta, kind = nil, nil
                if type(reflect_for_repl) == "function" then
                    meta, kind = reflect_for_repl(val)
                end
                if kind == "class" then
                    if type(meta) == "table" then
                        if type(meta.methods) == "table" then
                            for mname in pairs(meta.methods) do
                                add_if_match(matches, seen, mname, partial, include_dunder)
                            end
                        end
                        if sep == "." and type(meta.fields) == "table" then
                            for fname, finfo in pairs(meta.fields) do
                                if type(finfo) == "table" and finfo.static then
                                    add_if_match(matches, seen, fname, partial, include_dunder)
                                end
                            end
                        end
                        if type(meta.constructors) == "table" and #meta.constructors > 0 then
                            add_if_match(matches, seen, "new", partial, include_dunder)
                        end
                    end
                    local meta_list = sep == "." and CLASS_META_DOT_COMPLETIONS or CLASS_META_COLON_COMPLETIONS
                    for i = 1, #meta_list do
                        add_if_match(matches, seen, meta_list[i], partial, include_dunder)
                    end
                elseif kind == "ref" then
                    if type(meta) == "table" then
                        if type(meta.methods) == "table" then
                            for mname in pairs(meta.methods) do
                                add_if_match(matches, seen, mname, partial, include_dunder)
                            end
                        end
                        if sep == "." and type(meta.fields) == "table" then
                            for fname in pairs(meta.fields) do
                                add_if_match(matches, seen, fname, partial, include_dunder)
                            end
                        end
                    end
                    for i = 1, #REF_META_COMPLETIONS do
                        add_if_match(matches, seen, REF_META_COMPLETIONS[i], partial, include_dunder)
                    end
                elseif kind == "released" then
                    for i = 1, #REF_META_COMPLETIONS do
                        add_if_match(matches, seen, REF_META_COMPLETIONS[i], partial, include_dunder)
                    end
                elseif rawget(val, METHOD_KEY) ~= nil then
                    local m_st = rawget(val, METHOD_KEY)
                    local method_helpers
                    if m_st.ov ~= nil then
                        method_helpers = sep == "."
                            and { "hook", "sig", "signature" }
                            or { "hook", "signature" }
                    else
                        method_helpers = { "hook", "overload", "overloads" }
                    end
                    for i = 1, #method_helpers do
                        add_if_match(matches, seen, method_helpers[i], partial, include_dunder)
                    end
                else
                    -- Plain Lua table or table with a static method table __index (e.g. xml.node, scope):
                    -- iterate via next to avoid triggering any custom __pairs or function __index.
                    -- On colon (':') completion, only include callable entries.
                    local function is_callable_entry(entry)
                        if type(entry) == "function" then
                            return true
                        end
                        if type(entry) == "table" then
                            local emt = getmetatable(entry)
                            return type(emt) == "table" and rawget(emt, "__call") ~= nil
                        end
                        return false
                    end
                    local hide_internal = partial:sub(1, 1) ~= "_"
                    for k, v in next, val do
                        if is_identifier_key(k) then
                            local is_hidden_helper = (val == target and k:sub(1, 6) == "_repl_" and partial:sub(1, 5) ~= "_repl")
                                or (hide_internal and (k == "_node_methods" or k == "_db_methods" or k == "_stmt_methods"))
                            if not is_hidden_helper and (sep ~= ":" or is_callable_entry(v)) then
                                add_if_match(matches, seen, k, partial, include_dunder)
                            end
                        end
                    end
                    local mt = getmetatable(val)
                    local idx_tbl = type(mt) == "table" and rawget(mt, "__index") or nil
                    if type(idx_tbl) == "table" then
                        for k, v in next, idx_tbl do
                            if is_identifier_key(k) and (sep ~= ":" or is_callable_entry(v)) then
                                add_if_match(matches, seen, k, partial, include_dunder)
                            end
                        end
                    end
                end
            end
            t_sort(matches)
            local prefix = base .. sep
            local out = {}
            local cap = m_min(#matches, 200)
            for i = 1, cap do
                out[i] = prefix .. matches[i]
            end
            return json_encode(json_array(out))
        end

        if not text:match("^[A-Za-z0-9_]*$") then
            return "[]"
        end
        local include_internal = text:sub(1, 1) == "_"
        for k in next, _G do
            if is_identifier_key(k) then
                if include_internal or k:sub(1, 1) ~= "_" then
                    add_if_match(matches, seen, k, text, true)
                end
            end
        end
        for i = 1, #LUA_KEYWORDS do
            add_if_match(matches, seen, LUA_KEYWORDS[i], text, true)
        end
        t_sort(matches)
        local out = {}
        local cap = m_min(#matches, 200)
        for i = 1, cap do
            out[i] = matches[i]
        end
        return json_encode(json_array(out))
    end

    function target._repl_gc()
        collectgarbage("collect")
        local mem_kb = collectgarbage("count")
        local ref_count = 0
        for _ in pairs(live_refs) do
            ref_count = ref_count + 1
        end
        return json_encode({
            memory_kb = mem_kb,
            live_java_refs = ref_count,
        })
    end

    local function collect_hook_lines(raw_list)
        local lines = {}
        if type(raw_list) == "string" then
            for line in (raw_list .. "\n"):gmatch("(.-)\n") do
                if line ~= "" then
                    lines[#lines + 1] = line
                end
            end
        elseif type(raw_list) == "table" then
            for i = 1, rawlen(raw_list) do
                lines[#lines + 1] = safe_tostring(rawget(raw_list, i))
            end
        end
        return lines
    end

    function target._repl_hooks()
        local java_lines = {}
        local native_lines = {}
        local h = rawget(_G, "hook")
        local h_list = type(h) == "table" and rawget(h, "list") or nil
        if type(h_list) == "function" then
            local ok, list = pcall(h_list)
            if ok then
                java_lines = collect_hook_lines(list)
            end
        end
        local n = rawget(_G, "native")
        local n_list = type(n) == "table" and rawget(n, "list") or nil
        if type(n_list) == "function" then
            local ok, list = pcall(n_list)
            if ok then
                native_lines = collect_hook_lines(list)
            end
        end
        return json_encode({
            java = json_array(java_lines),
            native = json_array(native_lines),
        })
    end
end

local function install_xml_sugar()
    local x = rawget(_G, "xml")
    if type(x) ~= "table" or type(rawget(x, "prefs")) == "function" then
        return
    end

    local type = type
    local rawget = rawget
    local rawset = rawset
    local next = next
    local pairs = pairs
    local ipairs = ipairs
    local assert = assert
    local error = error
    local tonumber = tonumber
    local tostring = tostring
    local t_concat = table.concat
    local t_sort = table.sort
    local t_remove = table.remove
    local m_type = math.type
    local m_tointeger = math.tointeger

    local methods = rawget(x, "_node_methods")
    if type(methods) ~= "table" then
        methods = {}
        rawset(x, "_node_methods", methods)
    end

    function methods:set_text(str)
        assert(type(self) == "table", "expected XML node")
        assert(type(str) == "string", "set_text expects a string")
        self.children = { str }
        return self
    end

    function methods:set_attr(name, value)
        assert(type(self) == "table", "expected XML node")
        assert(type(name) == "string" and name ~= "", "set_attr expects a non-empty attribute name")
        if type(self.attrs) ~= "table" then
            self.attrs = {}
        end
        if value == nil then
            self.attrs[name] = nil
        else
            local tv = type(value)
            assert(tv == "string" or tv == "number" or tv == "boolean",
                   "set_attr value must be string, number, boolean, or nil")
            if tv == "number" then
                assert(value == value and value ~= math.huge and value ~= -math.huge,
                       "set_attr number must be finite")
                self.attrs[name] = (tostring(value):gsub(",", "."))
            else
                self.attrs[name] = tostring(value)
            end
        end
        return self
    end

    function methods:elements(tag)
        assert(type(self) == "table", "expected XML node")
        local out = {}
        local children = self.children
        if type(children) == "table" then
            for i = 1, #children do
                local c = children[i]
                if type(c) == "table" and (tag == nil or c.tag == tag) then
                    out[#out + 1] = c
                end
            end
        end
        return out
    end

    function methods:child(tag)
        assert(type(self) == "table", "expected XML node")
        local children = self.children
        if type(children) == "table" then
            for i = 1, #children do
                local c = children[i]
                if type(c) == "table" and (tag == nil or c.tag == tag) then
                    return c
                end
            end
        end
        return nil
    end

    function methods:add(child)
        assert(type(self) == "table", "expected XML node")
        assert(type(child) == "table" or type(child) == "string",
               "add expects an XML node table or string")
        if type(self.children) ~= "table" then
            self.children = {}
        end
        self.children[#self.children + 1] = child
        return self
    end

    function methods:remove(target_or_index)
        assert(type(self) == "table", "expected XML node")
        local children = self.children
        if type(children) ~= "table" then
            return nil
        end
        if type(target_or_index) == "number" and m_type(target_or_index) == "integer" then
            if target_or_index < 1 or target_or_index > #children then
                return nil
            end
            return t_remove(children, target_or_index)
        end
        for i = 1, #children do
            if children[i] == target_or_index then
                return t_remove(children, i)
            end
        end
        return nil
    end

    local function skip_sel_ws(path, pos, len)
        while pos <= len and path:sub(pos, pos):match("%s") do
            pos = pos + 1
        end
        return pos
    end

    local function parse_selector(path)
        assert(type(path) == "string" and path ~= "", "selector path must be a non-empty string")
        local steps = {}
        local pos = 1
        local len = #path
        while pos <= len do
            local recursive = false
            if path:sub(pos, pos + 1) == "//" then
                recursive = true
                pos = pos + 2
            elseif path:sub(pos, pos) == "/" then
                pos = pos + 1
            elseif #steps > 0 then
                error("expected '/' between XML selector steps: " .. path, 0)
            end
            if pos > len then
                error("trailing '/' in XML selector: " .. path, 0)
            end

            local tag_start = pos
            while pos <= len do
                local ch = path:sub(pos, pos)
                if ch == "/" or ch == "[" then
                    break
                end
                pos = pos + 1
            end
            local tag = path:sub(tag_start, pos - 1):match("^%s*(.-)%s*$")
            assert(tag ~= "", "empty tag name in XML selector: " .. path)

            local attrs = {}
            local index_pred = nil
            while pos <= len and path:sub(pos, pos) == "[" do
                pos = skip_sel_ws(path, pos + 1, len)
                if path:sub(pos, pos) == "@" then
                    pos = pos + 1
                    local k_start = pos
                    while pos <= len do
                        local ch = path:sub(pos, pos)
                        if ch == "=" or ch == "]" or ch:match("%s") then
                            break
                        end
                        pos = pos + 1
                    end
                    local k = path:sub(k_start, pos - 1)
                    assert(k ~= "", "empty attribute name in XML selector: " .. path)
                    pos = skip_sel_ws(path, pos, len)
                    local v = true -- existence check
                    if path:sub(pos, pos) == "=" then
                        pos = skip_sel_ws(path, pos + 1, len)
                        local q = path:sub(pos, pos)
                        if q == "'" or q == '"' then
                            pos = pos + 1
                            local v_start = pos
                            while pos <= len and path:sub(pos, pos) ~= q do
                                pos = pos + 1
                            end
                            assert(pos <= len and path:sub(pos, pos) == q,
                                   "unterminated quoted value in XML selector: " .. path)
                            v = path:sub(v_start, pos - 1)
                            pos = skip_sel_ws(path, pos + 1, len)
                        else
                            local v_start = pos
                            while pos <= len and path:sub(pos, pos) ~= "]" do
                                pos = pos + 1
                            end
                            v = path:sub(v_start, pos - 1):match("^%s*(.-)%s*$")
                        end
                    end
                    assert(path:sub(pos, pos) == "]", "expected ']' in XML selector: " .. path)
                    pos = pos + 1
                    attrs[#attrs + 1] = { key = k, value = v }
                else
                    assert(index_pred == nil, "duplicate index predicate in XML selector: " .. path)
                    local idx_start = pos
                    while pos <= len and path:sub(pos, pos) ~= "]" do
                        pos = pos + 1
                    end
                    assert(path:sub(pos, pos) == "]", "expected ']' in XML selector: " .. path)
                    local raw_idx = path:sub(idx_start, pos - 1):match("^%s*(.-)%s*$")
                    local parsed = m_tointeger(tonumber(raw_idx))
                    assert(parsed ~= nil and parsed >= 1,
                           "XML selector index must be a positive integer: " .. raw_idx)
                    index_pred = parsed
                    pos = pos + 1
                end
            end

            steps[#steps + 1] = {
                recursive = recursive,
                tag = tag,
                attrs = attrs,
                index = index_pred,
            }
        end
        return steps
    end

    local function node_matches_step(node, step)
        if type(node) ~= "table" then
            return false
        end
        if step.tag ~= "*" and node.tag ~= step.tag then
            return false
        end
        if #step.attrs > 0 then
            local nattrs = node.attrs
            if type(nattrs) ~= "table" then
                return false
            end
            for i = 1, #step.attrs do
                local pred = step.attrs[i]
                local actual = nattrs[pred.key]
                if pred.value == true then
                    if actual == nil then
                        return false
                    end
                elseif actual ~= pred.value then
                    return false
                end
            end
        end
        return true
    end

    local function collect_descendants(node, step, out, matched_seen, visited, depth)
        if depth > 64 or type(node) ~= "table" or type(node.children) ~= "table" then
            return
        end
        for i = 1, #node.children do
            local c = node.children[i]
            if type(c) == "table" and not visited[c] then
                visited[c] = true
                if node_matches_step(c, step) and not matched_seen[c] then
                    matched_seen[c] = true
                    out[#out + 1] = c
                end
                collect_descendants(c, step, out, matched_seen, visited, depth + 1)
            end
        end
    end

    function methods:find_all(path)
        assert(type(self) == "table", "expected XML node")
        local steps = parse_selector(path)
        local current = { self }
        for s = 1, #steps do
            local step = steps[s]
            local next_nodes = {}
            local seen = {}
            for i = 1, #current do
                local parent = current[i]
                if step.recursive then
                    local found = {}
                    collect_descendants(parent, step, found, {}, { [parent] = true }, 0)
                    if step.index ~= nil then
                        local pick = found[step.index]
                        if pick ~= nil and not seen[pick] then
                            seen[pick] = true
                            next_nodes[#next_nodes + 1] = pick
                        end
                    else
                        for j = 1, #found do
                            if not seen[found[j]] then
                                seen[found[j]] = true
                                next_nodes[#next_nodes + 1] = found[j]
                            end
                        end
                    end
                elseif type(parent.children) == "table" then
                    local matched_here = {}
                    for j = 1, #parent.children do
                        local c = parent.children[j]
                        if node_matches_step(c, step) then
                            matched_here[#matched_here + 1] = c
                        end
                    end
                    if step.index ~= nil then
                        local pick = matched_here[step.index]
                        if pick ~= nil and not seen[pick] then
                            seen[pick] = true
                            next_nodes[#next_nodes + 1] = pick
                        end
                    else
                        for j = 1, #matched_here do
                            if not seen[matched_here[j]] then
                                seen[matched_here[j]] = true
                                next_nodes[#next_nodes + 1] = matched_here[j]
                            end
                        end
                    end
                end
            end
            current = next_nodes
            if #current == 0 then
                break
            end
        end
        return current
    end

    function methods:find(path)
        local matches = self:find_all(path)
        return matches[1]
    end

    local function node_to_plain(node, depth, ancestors, budget)
        if ancestors[node] then
            error("cyclic XML node reference", 0)
        end
        if depth > 64 then
            error("XML node depth limit exceeded", 0)
        end
        budget.nodes = budget.nodes + 1
        if budget.nodes > 65536 then
            error("XML node count limit exceeded", 0)
        end
        ancestors[node] = true
        local attrs = {}
        if type(node.attrs) == "table" then
            for k, v in pairs(node.attrs) do
                attrs[k] = v
            end
        end
        local children = {}
        local text_parts = {}
        local has_elem = false
        if type(node.children) == "table" then
            for i = 1, #node.children do
                local c = node.children[i]
                if type(c) == "table" then
                    has_elem = true
                    children[#children + 1] = node_to_plain(c, depth + 1, ancestors, budget)
                elseif type(c) == "string" then
                    text_parts[#text_parts + 1] = c
                end
            end
        end
        ancestors[node] = nil
        local out = { tag = node.tag, attrs = attrs }
        if has_elem then
            out.children = children
            if #text_parts > 0 then
                out.text = t_concat(text_parts)
            end
        else
            out.text = t_concat(text_parts)
        end
        return out
    end

    function methods:to_table()
        assert(type(self) == "table", "expected XML node")
        return node_to_plain(self, 0, {}, { nodes = 0 })
    end

    local function require_fs(op)
        local f = rawget(_G, "fs")
        assert(type(f) == "table" and type(f[op]) == "function",
               "fs." .. op .. " is unavailable in this Lua session")
        return f[op]
    end

    function x.load(path, opts)
        assert(type(path) == "string" and path ~= "", "xml.load expects a file path")
        local read_fn = require_fs("read")
        local content = read_fn(path)
        return x.parse(content, opts)
    end
    x.parse_file = x.load

    function x.save(path, node, opts)
        assert(type(path) == "string" and path ~= "", "xml.save expects a file path")
        local write_fn = require_fs("write")
        local encoded = x.encode(node, opts)
        return write_fn(path, encoded)
    end
    x.write_file = x.save

    -- Android SharedPreferences (<map>...</map>) sugar
    function x.prefs(xml_or_path)
        assert(type(xml_or_path) == "string" and xml_or_path ~= "",
               "xml.prefs expects XML text or a file path")
        local probe = xml_or_path
        if probe:sub(1, 3) == "\239\187\191" then
            probe = probe:sub(4)
        end
        local root
        if probe:match("^%s*<") then
            root = x.parse(xml_or_path)
        else
            root = x.load(xml_or_path)
        end
        assert(root.tag == "map",
               "xml.prefs expected SharedPreferences <map> root, got <" .. tostring(root.tag) .. ">")
        local result = {}
        local elems = root:elements()
        for i = 1, #elems do
            local el = elems[i]
            local name = el:attr("name")
            if name ~= nil then
                local tag = el.tag
                if tag == "string" then
                    result[name] = el:text()
                elseif tag == "int" or tag == "long" then
                    local raw = el:attr("value")
                    local num = raw ~= nil and tonumber(raw) or nil
                    result[name] = (num ~= nil and m_tointeger(num)) or num
                elseif tag == "float" or tag == "double" then
                    local raw = el:attr("value")
                    if raw == "NaN" then
                        result[name] = 0 / 0
                    elseif raw == "Infinity" then
                        result[name] = math.huge
                    elseif raw == "-Infinity" then
                        result[name] = -math.huge
                    else
                        result[name] = raw ~= nil and tonumber(raw) or nil
                    end
                elseif tag == "boolean" then
                    result[name] = (el:attr("value") == "true")
                elseif tag == "set" then
                    local list = {}
                    local items = el:elements("string")
                    for j = 1, #items do
                        list[j] = items[j]:text()
                    end
                    result[name] = list
                end
            end
        end
        return result
    end

    function x.encode_prefs(tbl, opts)
        assert(type(tbl) == "table", "xml.encode_prefs expects a table")
        local keys = {}
        for k in next, tbl do
            assert(type(k) == "string", "SharedPreferences keys must be strings")
            keys[#keys + 1] = k
        end
        t_sort(keys)
        local root = x.node("map")
        for i = 1, #keys do
            local k = keys[i]
            local v = tbl[k]
            local tv = type(v)
            if tv == "string" then
                root:add(x.node("string", { name = k }, v))
            elseif tv == "boolean" then
                root:add(x.node("boolean", { name = k, value = v and "true" or "false" }))
            elseif tv == "number" then
                if m_type(v) == "integer" then
                    local tag = (v >= -2147483648 and v <= 2147483647) and "int" or "long"
                    root:add(x.node(tag, { name = k, value = tostring(v) }))
                else
                    local fstr
                    if v ~= v then
                        fstr = "NaN"
                    elseif v == math.huge then
                        fstr = "Infinity"
                    elseif v == -math.huge then
                        fstr = "-Infinity"
                    else
                        fstr = (tostring(v):gsub(",", "."))
                    end
                    root:add(x.node("float", { name = k, value = fstr }))
                end
            elseif tv == "table" then
                local n = #v
                local count = 0
                for sk, sv in next, v do
                    count = count + 1
                    assert(type(sk) == "number" and m_type(sk) == "integer" and sk >= 1 and sk <= n,
                           "SharedPreferences <set> for key '" .. k .. "' must be a contiguous string array")
                    assert(type(sv) == "string",
                           "SharedPreferences <set> elements for key '" .. k .. "' must be strings")
                end
                assert(count == n,
                       "SharedPreferences <set> for key '" .. k .. "' must not be sparse")
                local set_node = x.node("set", { name = k })
                for j = 1, n do
                    set_node:add(x.node("string", {}, v[j]))
                end
                root:add(set_node)
            else
                error("unsupported SharedPreferences value type for key '" .. k .. "': " .. tv, 0)
            end
        end
        local enc_opts = { declaration = true, indent = "  " }
        if type(opts) == "table" then
            if opts.declaration ~= nil then enc_opts.declaration = opts.declaration end
            if opts.indent ~= nil then enc_opts.indent = opts.indent end
        end
        return x.encode(root, enc_opts)
    end

    function x.save_prefs(path, tbl, opts)
        assert(type(path) == "string" and path ~= "", "xml.save_prefs expects a file path")
        local write_fn = require_fs("write")
        return write_fn(path, x.encode_prefs(tbl, opts))
    end
end

local function install_sqlite_sugar()
    local sq = rawget(_G, "sqlite")
    if type(sq) ~= "table" or type(rawget(sq, "with")) == "function" then
        return
    end

    local type = type
    local rawget = rawget
    local rawequal = rawequal
    local next = next
    local pairs = pairs
    local pcall = pcall
    local assert = assert
    local error = error
    local tostring = tostring
    local t_concat = table.concat
    local t_sort = table.sort
    local t_pack = table.pack
    local t_unpack = table.unpack
    local m_type = math.type
    local m_huge = math.huge

    local json_table = rawget(_G, "json")
    local pinned_json_null = type(json_table) == "table" and rawget(json_table, "null") or nil

    local db_methods = rawget(sq, "_db_methods")
    if type(db_methods) ~= "table" then
        db_methods = {}
        rawset(sq, "_db_methods", db_methods)
    end

    local function quote_ident(name)
        assert(type(name) == "string" and name ~= "" and not name:find("\0", 1, true),
               "SQLite identifier must be a non-empty string without NUL bytes")
        return '"' .. (name:gsub('"', '""')) .. '"'
    end

    local function is_sql_null(v)
        if v == nil then
            return true
        end
        if rawequal(v, sq.NULL) then
            return true
        end
        if pinned_json_null ~= nil then
            return rawequal(v, pinned_json_null)
        end
        local j = rawget(_G, "json")
        if type(j) == "table" then
            local jn = rawget(j, "null")
            if jn ~= nil then
                pinned_json_null = jn
                return rawequal(v, jn)
            end
        end
        return false
    end

    local function sorted_keys(tbl)
        local keys = {}
        for k in next, tbl do
            assert(type(k) == "string" and k ~= "" and not k:find("\0", 1, true),
                   "SQLite column name must be a non-empty string without NUL bytes")
            keys[#keys + 1] = k
        end
        t_sort(keys)
        return keys
    end

    local function build_where_clause(where, where_params, params_out, param_offset)
        param_offset = param_offset or 0
        if where == nil then
            return nil
        end
        if type(where) == "table" then
            local keys = sorted_keys(where)
            if #keys == 0 then
                return nil
            end
            local clauses = {}
            local pos = 0
            for i = 1, #keys do
                local k = keys[i]
                local v = where[k]
                if is_sql_null(v) then
                    clauses[i] = quote_ident(k) .. " IS NULL"
                else
                    clauses[i] = quote_ident(k) .. " = ?"
                    pos = pos + 1
                    params_out[param_offset + pos] = v
                end
            end
            return t_concat(clauses, " AND ")
        elseif type(where) == "string" then
            assert(not where:match("^%s*$"), "WHERE clause string must not be empty")
            assert(not where:find("\0", 1, true), "WHERE clause must not contain NUL bytes")
            if param_offset > 0 then
                assert(not where:find("%?%d"),
                       "numbered ?N placeholders are not supported in db:update string WHERE; use ? or :name")
            end
            if where_params ~= nil then
                assert(type(where_params) == "table", "where_params must be a table")
                for k, v in next, where_params do
                    if type(k) == "number" and m_type(k) == "integer" and k >= 1 then
                        params_out[param_offset + k] = v
                    elseif type(k) == "string" and k ~= "" then
                        params_out[k] = v
                    else
                        error("where_params keys must be positive integers or non-empty strings", 0)
                    end
                end
            end
            return where
        end
        error("WHERE clause must be a table, SQL string, or nil", 0)
    end

    local function format_order_term(col, dir_raw)
        local dir_trim = (dir_raw or ""):match("^%s*(.-)%s*$"):upper()
        if dir_trim == "" then
            return quote_ident(col)
        elseif dir_trim == "ASC" or dir_trim == "DESC" then
            return quote_ident(col) .. " " .. dir_trim
        end
        error("invalid ORDER BY direction: " .. dir_trim, 0)
    end

    local function build_order_clause(order)
        if order == nil then
            return nil
        end
        local out = {}
        if type(order) == "string" then
            assert(not order:match("^%s*$"), "order string must not be empty")
            for part in (order .. ","):gmatch("(.-),") do
                local trimmed = part:match("^%s*(.-)%s*$")
                if trimmed ~= "" then
                    local col, dir = trimmed:match("^([A-Za-z_][A-Za-z0-9_]*)(.*)$")
                    assert(col ~= nil, "invalid ORDER BY term: " .. trimmed)
                    out[#out + 1] = format_order_term(col, dir)
                end
            end
        elseif type(order) == "table" then
            local count = 0
            for k in next, order do
                assert(type(k) == "number" and m_type(k) == "integer" and k >= 1,
                       "order table must be a contiguous array")
                count = count + 1
            end
            assert(count > 0 and count == #order, "order table must be a non-empty contiguous array")
            for i = 1, count do
                local entry = order[i]
                if type(entry) == "string" then
                    local trimmed = entry:match("^%s*(.-)%s*$")
                    local col, dir = trimmed:match("^([A-Za-z_][A-Za-z0-9_]*)(.*)$")
                    assert(col ~= nil, "invalid ORDER BY term: " .. trimmed)
                    out[#out + 1] = format_order_term(col, dir)
                elseif type(entry) == "table" then
                    assert(type(entry[1]) == "string" and entry[1] ~= "",
                           "order tuple must start with a column name string")
                    assert(entry[2] == nil or type(entry[2]) == "string",
                           "order tuple direction must be a string or nil")
                    out[#out + 1] = format_order_term(entry[1], entry[2])
                else
                    error("order entries must be strings or { column, direction } tables", 0)
                end
            end
        else
            error("order option must be a string or table", 0)
        end
        assert(#out > 0, "order option must specify at least one column")
        return t_concat(out, ", ")
    end

    local function merge_find_opts(where, opts)
        local merged = {}
        if opts ~= nil then
            assert(type(opts) == "table", "opts must be a table")
            for k, v in next, opts do
                if type(k) == "number" then
                    error("find/first opts must be an options table (pass SQL parameters via { params = ... })", 0)
                end
                merged[k] = v
            end
        end
        merged.where = where
        return merged
    end

    -- Top-level ergonomic constructors
    function sq.memory(opts)
        return sq.open(":memory:", opts)
    end

    function sq.open_ro(path, opts)
        if opts == nil then
            return sq.open(path, "ro")
        end
        assert(type(opts) == "table", "sqlite.open_ro expects an optional options table")
        local copy = {}
        for k, v in pairs(opts) do
            copy[k] = v
        end
        copy.mode = "ro"
        return sq.open(path, copy)
    end

    function sq.with(path, mode_or_fn, fn)
        local mode_or_opts = nil
        local callback = fn
        if type(mode_or_fn) == "function" and fn == nil then
            callback = mode_or_fn
        else
            mode_or_opts = mode_or_fn
        end
        assert(type(callback) == "function", "sqlite.with expects a callback function")
        local db = sq.open(path, mode_or_opts)
        local results = t_pack(pcall(callback, db))
        if db:is_open() then
            db:close()
        end
        if not results[1] then
            error(results[2], 0)
        end
        return t_unpack(results, 2, results.n)
    end

    -- Introspection helpers
    function db_methods:tables()
        local rows = self:query(
            "SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%' ORDER BY name"
        )
        local out = {}
        for i = 1, #rows do
            out[i] = rows[i].name
        end
        return out
    end

    function db_methods:columns(table_name)
        return self:query("PRAGMA table_info(" .. quote_ident(table_name) .. ")")
    end

    function db_methods:schema(table_name)
        if table_name ~= nil then
            assert(type(table_name) == "string" and table_name ~= "", "table_name must be a string")
            return self:scalar(
                "SELECT sql FROM sqlite_master WHERE type = 'table' AND name = ?",
                { table_name }
            )
        end
        local rows = self:query(
            "SELECT name, sql FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%' ORDER BY name"
        )
        local out = {}
        for i = 1, #rows do
            out[rows[i].name] = rows[i].sql
        end
        return out
    end

    function db_methods:pragma(name, value)
        assert(type(name) == "string" and name:match("^[A-Za-z_][A-Za-z0-9_]*$") ~= nil,
               "invalid PRAGMA name: " .. tostring(name))
        if value == nil then
            return self:scalar("PRAGMA " .. name)
        end
        local tv = type(value)
        local rendered
        if tv == "boolean" then
            rendered = value and "ON" or "OFF"
        elseif tv == "number" then
            assert(value == value and value ~= m_huge and value ~= -m_huge,
                   "PRAGMA numeric value must be finite")
            rendered = (tostring(value):gsub(",", "."))
        elseif tv == "string" then
            assert(not value:find("\0", 1, true), "PRAGMA string value must not contain NUL bytes")
            rendered = "'" .. (value:gsub("'", "''")) .. "'"
        else
            error("unsupported PRAGMA value type: " .. tv, 0)
        end
        self:exec("PRAGMA " .. name .. " = " .. rendered, {})
        return self:scalar("PRAGMA " .. name)
    end

    -- CRUD sugar (all values strictly parameterized, all identifiers quoted)
    function db_methods:insert(table_name, row_or_rows)
        assert(type(row_or_rows) == "table", "db:insert expects a row table or array of rows")
        local tbl_q = quote_ident(table_name)
        if type(row_or_rows[1]) == "table" then
            local count = 0
            for k in next, row_or_rows do
                assert(type(k) == "number" and m_type(k) == "integer" and k >= 1,
                       "batch insert expects a contiguous array of row tables")
                count = count + 1
            end
            assert(count > 0 and count == #row_or_rows,
                   "batch insert array must not contain holes")
            return self:transaction(function()
                local inserted = 0
                local cached_key = nil
                local cached_stmt = nil
                local ok_batch, batch_err = pcall(function()
                    for r = 1, count do
                        local row = row_or_rows[r]
                        assert(type(row) == "table", "batch insert item must be a table")
                        local keys = sorted_keys(row)
                        assert(#keys > 0, "insert row must not be empty")
                        local sig = t_concat(keys, "\0")
                        if sig ~= cached_key then
                            if cached_stmt ~= nil then
                                cached_stmt:close()
                                cached_stmt = nil
                            end
                            local qcols, ph = {}, {}
                            for i = 1, #keys do
                                qcols[i] = quote_ident(keys[i])
                                ph[i] = "?"
                            end
                            local sql = "INSERT INTO " .. tbl_q .. " (" .. t_concat(qcols, ", ") ..
                                        ") VALUES (" .. t_concat(ph, ", ") .. ")"
                            cached_stmt = self:prepare(sql)
                            cached_key = sig
                        end
                        local vals = {}
                        for i = 1, #keys do
                            vals[i] = row[keys[i]]
                        end
                        cached_stmt:run(vals)
                        inserted = inserted + 1
                    end
                end)
                if cached_stmt ~= nil then
                    cached_stmt:close()
                end
                if not ok_batch then
                    error(batch_err, 0)
                end
                return inserted
            end)
        end

        local keys = sorted_keys(row_or_rows)
        assert(#keys > 0, "insert row must not be empty")
        local qcols, ph, vals = {}, {}, {}
        for i = 1, #keys do
            qcols[i] = quote_ident(keys[i])
            ph[i] = "?"
            vals[i] = row_or_rows[keys[i]]
        end
        local sql = "INSERT INTO " .. tbl_q .. " (" .. t_concat(qcols, ", ") ..
                    ") VALUES (" .. t_concat(ph, ", ") .. ")"
        local _, rowid = self:exec(sql, vals)
        return rowid
    end

    function db_methods:upsert(table_name, row, conflict_cols)
        assert(type(row) == "table", "db:upsert expects a row table")
        local keys = sorted_keys(row)
        assert(#keys > 0, "upsert row must not be empty")

        local conflict_list = {}
        local conflict_set = {}
        if type(conflict_cols) == "string" then
            conflict_list[1] = quote_ident(conflict_cols)
            conflict_set[conflict_cols] = true
        elseif type(conflict_cols) == "table" and #conflict_cols > 0 then
            for i = 1, #conflict_cols do
                conflict_list[i] = quote_ident(conflict_cols[i])
                conflict_set[conflict_cols[i]] = true
            end
        else
            error("db:upsert expects conflict_cols as a column name string or non-empty array", 0)
        end

        local qcols, ph, vals, updates = {}, {}, {}, {}
        for i = 1, #keys do
            local k = keys[i]
            local qk = quote_ident(k)
            qcols[i] = qk
            ph[i] = "?"
            vals[i] = row[k]
            if not conflict_set[k] then
                updates[#updates + 1] = qk .. " = excluded." .. qk
            end
        end

        local action = #updates > 0
            and ("DO UPDATE SET " .. t_concat(updates, ", "))
            or "DO NOTHING"
        local sql = "INSERT INTO " .. quote_ident(table_name) .. " (" .. t_concat(qcols, ", ") ..
                    ") VALUES (" .. t_concat(ph, ", ") .. ") ON CONFLICT(" ..
                    t_concat(conflict_list, ", ") .. ") " .. action
        return self:exec(sql, vals)
    end

    function db_methods:update(table_name, values, where, where_params)
        assert(type(values) == "table", "db:update expects a values table")
        local keys = sorted_keys(values)
        assert(#keys > 0, "db:update values table must not be empty")
        assert(where ~= nil, "db:update requires a WHERE clause (pass true to update all rows)")

        local sets, params = {}, {}
        for i = 1, #keys do
            local k = keys[i]
            sets[i] = quote_ident(k) .. " = ?"
            params[i] = values[k]
        end

        local sql = "UPDATE " .. quote_ident(table_name) .. " SET " .. t_concat(sets, ", ")
        if where ~= true then
            local clause = build_where_clause(where, where_params, params, #keys)
            assert(clause ~= nil, "db:update WHERE table must not be empty (pass true to update all rows)")
            sql = sql .. " WHERE " .. clause
        end
        local changes = self:exec(sql, params)
        return changes
    end

    function db_methods:delete(table_name, where, where_params)
        assert(where ~= nil, "db:delete requires a WHERE clause (pass true to delete all rows)")
        local params = {}
        local sql = "DELETE FROM " .. quote_ident(table_name)
        if where ~= true then
            local clause = build_where_clause(where, where_params, params, 0)
            assert(clause ~= nil, "db:delete WHERE table must not be empty (pass true to delete all rows)")
            sql = sql .. " WHERE " .. clause
        end
        local changes = self:exec(sql, params)
        return changes
    end

    function db_methods:select(table_name, opts)
        opts = opts or {}
        assert(type(opts) == "table", "db:select expects an optional options table")
        local col_sql = "*"
        if opts.cols ~= nil then
            if type(opts.cols) == "string" then
                col_sql = quote_ident(opts.cols)
            elseif type(opts.cols) == "table" then
                local count = 0
                for k in next, opts.cols do
                    assert(type(k) == "number" and m_type(k) == "integer" and k >= 1,
                           "cols table must be a contiguous array of column names")
                    count = count + 1
                end
                assert(count > 0 and count == #opts.cols,
                       "cols table must be a non-empty contiguous array of column names")
                local qcols = {}
                for i = 1, count do
                    qcols[i] = quote_ident(opts.cols[i])
                end
                col_sql = t_concat(qcols, ", ")
            else
                error("cols option must be a string or non-empty array of strings", 0)
            end
        end

        local params = {}
        local sql = "SELECT " .. col_sql .. " FROM " .. quote_ident(table_name)
        if opts.where ~= nil then
            local clause = build_where_clause(opts.where, opts.params, params, 0)
            if clause ~= nil then
                sql = sql .. " WHERE " .. clause
            end
        end
        local order_sql = build_order_clause(opts.order)
        if order_sql ~= nil then
            sql = sql .. " ORDER BY " .. order_sql
        end
        if opts.limit ~= nil then
            assert(type(opts.limit) == "number" and m_type(opts.limit) == "integer" and opts.limit >= 0,
                   "limit must be a non-negative integer")
            sql = sql .. " LIMIT " .. tostring(opts.limit)
        end
        if opts.offset ~= nil then
            assert(type(opts.offset) == "number" and m_type(opts.offset) == "integer" and opts.offset >= 0,
                   "offset must be a non-negative integer")
            if opts.limit == nil then
                sql = sql .. " LIMIT -1"
            end
            sql = sql .. " OFFSET " .. tostring(opts.offset)
        end

        local query_opts = nil
        if opts.mode ~= nil or opts.max_rows ~= nil or opts.null ~= nil then
            query_opts = { mode = opts.mode, max_rows = opts.max_rows, null = opts.null }
        end
        return self:query(sql, next(params) ~= nil and params or nil, query_opts)
    end

    function db_methods:find(table_name, where, opts)
        return self:select(table_name, merge_find_opts(where, opts))
    end

    function db_methods:first(table_name, where, opts)
        local merged = merge_find_opts(where, opts)
        merged.limit = 1
        local rows = self:select(table_name, merged)
        return rows[1]
    end

    function db_methods:count(table_name, where, where_params)
        local params = {}
        local sql = "SELECT COUNT(*) FROM " .. quote_ident(table_name)
        if where ~= nil then
            local clause = build_where_clause(where, where_params, params, 0)
            if clause ~= nil then
                sql = sql .. " WHERE " .. clause
            end
        end
        return self:scalar(sql, next(params) ~= nil and params or nil)
    end
end

install_java_resource_guards()
install_resource_scope()
install_xml_sugar()
install_sqlite_sugar()
install_repl_runtime()
