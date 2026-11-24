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
