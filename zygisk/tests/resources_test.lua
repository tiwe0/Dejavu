-- Host regression tests for zygisk/config/resources.lua:
--   1) Automatic GC (__gc) and Lua 5.4 <close> release of Java GlobalRef handles
--   2) Manual release, detach, use-after-release, loader .. ":Class", java.scope()
--   3) Frida-style java.use(), direct obj:method(...), static dot/colon calls, constructors
--   4) Proxy arguments (instance proxy, Class<?> class proxy, Lua string -> String/CharSequence/Object)
--   5) Overload scoring (exact / subclass via IsInstanceOf / Object / int vs long / ambiguity + :overload)
--   6) Static & instance fields, __cast views (non-double-release + shadowed fields + superclass calls)
--   7) Java arrays & byte[] (java.bytes, java.array, #arr, __get/__set/__bytes/__to_table)
--   8) Java method name collision safety (e.g. WakeLock:release() vs wl:__release())
--   9) dejavu.scope() LIFO cleanup + retryable busy RPC

local root = assert(arg[1], "usage: lua resources_test.lua <zygisk-dir>")

-- Minimal JSON decoder for standalone host lua binary (the real agent provides built-in C json).
local function json_decode(str)
    local pos = 1
    local len = #str
    local function ws()
        while pos <= len do
            local c = str:sub(pos, pos)
            if c == " " or c == "\t" or c == "\r" or c == "\n" then
                pos = pos + 1
            else
                break
            end
        end
    end
    local parse_val
    local function parse_str()
        assert(str:sub(pos, pos) == '"')
        pos = pos + 1
        local out = {}
        while pos <= len do
            local c = str:sub(pos, pos)
            pos = pos + 1
            if c == '"' then
                return table.concat(out)
            elseif c == "\\" then
                local e = str:sub(pos, pos)
                pos = pos + 1
                if e == '"' or e == "\\" or e == "/" then out[#out + 1] = e
                elseif e == "n" then out[#out + 1] = "\n"
                elseif e == "r" then out[#out + 1] = "\r"
                elseif e == "t" then out[#out + 1] = "\t"
                else error("unsupported escape: " .. e) end
            else
                out[#out + 1] = c
            end
        end
        error("unterminated string")
    end
    parse_val = function()
        ws()
        local c = str:sub(pos, pos)
        if c == '"' then
            return parse_str()
        elseif c == "{" then
            pos = pos + 1
            ws()
            local obj = {}
            if str:sub(pos, pos) == "}" then pos = pos + 1; return obj end
            while true do
                ws()
                local k = parse_str()
                ws()
                assert(str:sub(pos, pos) == ":")
                pos = pos + 1
                obj[k] = parse_val()
                ws()
                local sep = str:sub(pos, pos)
                pos = pos + 1
                if sep == "}" then return obj end
                assert(sep == ",")
            end
        elseif c == "[" then
            pos = pos + 1
            ws()
            local arr = {}
            if str:sub(pos, pos) == "]" then pos = pos + 1; return arr end
            while true do
                arr[#arr + 1] = parse_val()
                ws()
                local sep = str:sub(pos, pos)
                pos = pos + 1
                if sep == "]" then return arr end
                assert(sep == ",")
            end
        elseif str:sub(pos, pos + 3) == "true" then
            pos = pos + 4
            return true
        elseif str:sub(pos, pos + 4) == "false" then
            pos = pos + 5
            return false
        elseif str:sub(pos, pos + 3) == "null" then
            pos = pos + 4
            return nil
        else
            local num = str:match("^-?%d+%.?%d*", pos)
            assert(num, "invalid json at " .. pos)
            pos = pos + #num
            return tonumber(num)
        end
    end
    return parse_val()
end

local JSON_NULL = setmetatable({}, { __tostring = function() return "json.null" end })
json = {
    decode = json_decode,
    null = JSON_NULL,
    array = function()
        return setmetatable({}, { __is_json_array = true })
    end,
}

-- Mock JVM object table and class metadata.
local next_id = 0
local live = {}
local obj_class = {}
local obj_data = {}
local release_counts = {}
local last_call = nil
local static_fields = {
    ["android.os.Build.MODEL"] = "Pixel 9",
    ["android.os.Build.DEBUG"] = false,
}
local instance_fields = {}

local function alloc_handle(cname, data)
    next_id = next_id + 1
    live[next_id] = true
    obj_class[next_id] = cname or "java.lang.Object"
    obj_data[next_id] = data or {}
    return next_id
end

local REFLECT_META = {
    ["android.app.ActivityThread"] = [[{
        "name":"android.app.ActivityThread",
        "constructors":[],
        "methods":{
            "currentApplication":[{"sig":"()Landroid/app/Application;","static":true,"params":[],"ret":"Landroid/app/Application;"}]
        },
        "fields":{}
    }]],
    ["android.app.Application"] = [[{
        "name":"android.app.Application",
        "constructors":[],
        "methods":{
            "getPackageName":[{"sig":"()Ljava/lang/String;","static":false,"params":[],"ret":"Ljava/lang/String;"}],
            "getPackageManager":[{"sig":"()Landroid/content/pm/PackageManager;","static":false,"params":[],"ret":"Landroid/content/pm/PackageManager;"}]
        },
        "fields":{}
    }]],
    ["android.content.pm.PackageManager"] = [[{
        "name":"android.content.pm.PackageManager",
        "constructors":[],
        "methods":{
            "getLaunchIntentForPackage":[{"sig":"(Ljava/lang/String;)Landroid/content/Intent;","static":false,"params":["Ljava/lang/String;"],"ret":"Landroid/content/Intent;"}]
        },
        "fields":{}
    }]],
    ["android.content.Intent"] = [[{
        "name":"android.content.Intent",
        "constructors":[
            {"sig":"()V","params":[]},
            {"sig":"(Landroid/content/Context;Ljava/lang/Class;)V","params":["Landroid/content/Context;","Ljava/lang/Class;"]}
        ],
        "methods":{
            "getAction":[{"sig":"()Ljava/lang/String;","static":false,"params":[],"ret":"Ljava/lang/String;"}]
        },
        "fields":{}
    }]],
    ["com.example.MainActivity"] = [[{
        "name":"com.example.MainActivity",
        "constructors":[{"sig":"()V","params":[]}],
        "methods":{},
        "fields":{}
    }]],
    ["android.os.Build"] = [[{
        "name":"android.os.Build",
        "constructors":[],
        "methods":{},
        "fields":{
            "MODEL":{"sig":"Ljava/lang/String;","static":true},
            "DEBUG":{"sig":"Z","static":true}
        }
    }]],
    ["java.lang.StringBuilder"] = [[{
        "name":"java.lang.StringBuilder",
        "constructors":[
            {"sig":"()V","params":[]},
            {"sig":"(I)V","params":["I"]},
            {"sig":"(Ljava/lang/String;)V","params":["Ljava/lang/String;"]}
        ],
        "methods":{
            "append":[
                {"sig":"(Ljava/lang/String;)Ljava/lang/StringBuilder;","static":false,"params":["Ljava/lang/String;"],"ret":"Ljava/lang/StringBuilder;"},
                {"sig":"(I)Ljava/lang/StringBuilder;","static":false,"params":["I"],"ret":"Ljava/lang/StringBuilder;"}
            ],
            "toString":[{"sig":"()Ljava/lang/String;","static":false,"params":[],"ret":"Ljava/lang/String;"}]
        },
        "fields":{}
    }]],
    ["java.lang.String"] = [[{
        "name":"java.lang.String",
        "constructors":[],
        "methods":{
            "valueOf":[
                {"sig":"(I)Ljava/lang/String;","static":true,"params":["I"],"ret":"Ljava/lang/String;"},
                {"sig":"(J)Ljava/lang/String;","static":true,"params":["J"],"ret":"Ljava/lang/String;"},
                {"sig":"(D)Ljava/lang/String;","static":true,"params":["D"],"ret":"Ljava/lang/String;"},
                {"sig":"(Z)Ljava/lang/String;","static":true,"params":["Z"],"ret":"Ljava/lang/String;"},
                {"sig":"(Ljava/lang/Object;)Ljava/lang/String;","static":true,"params":["Ljava/lang/Object;"],"ret":"Ljava/lang/String;"}
            ]
        },
        "fields":{}
    }]],
    ["com.demo.WakeLock"] = [[{
        "name":"com.demo.WakeLock",
        "constructors":[{"sig":"()V","params":[]}],
        "methods":{
            "release":[{"sig":"()V","static":false,"params":[],"ret":"V"}],
            "equals":[{"sig":"(Ljava/lang/Object;)Z","static":false,"params":["Ljava/lang/Object;"],"ret":"Z"}]
        },
        "fields":{}
    }]],
    ["com.demo.Base"] = [[{
        "name":"com.demo.Base",
        "constructors":[{"sig":"()V","params":[]}],
        "methods":{
            "secret":[{"sig":"()Ljava/lang/String;","static":false,"params":[],"ret":"Ljava/lang/String;"}]
        },
        "fields":{
            "tag":{"sig":"Ljava/lang/String;","static":false},
            "count":{"sig":"I","static":false}
        }
    }]],
    ["com.demo.Sub"] = [[{
        "name":"com.demo.Sub",
        "constructors":[{"sig":"()V","params":[]}],
        "methods":{
            "handle":[
                {"sig":"(Lcom/demo/Base;)Ljava/lang/String;","static":false,"params":["Lcom/demo/Base;"],"ret":"Ljava/lang/String;"},
                {"sig":"(Ljava/lang/Object;)Ljava/lang/String;","static":false,"params":["Ljava/lang/Object;"],"ret":"Ljava/lang/String;"}
            ],
            "ambig":[
                {"sig":"(Ljava/lang/String;)Ljava/lang/String;","static":false,"params":["Ljava/lang/String;"],"ret":"Ljava/lang/String;"},
                {"sig":"(Ljava/io/File;)Ljava/lang/String;","static":false,"params":["Ljava/io/File;"],"ret":"Ljava/lang/String;"}
            ]
        },
        "fields":{
            "tag":{"sig":"Ljava/lang/String;","static":false},
            "count":{"sig":"I","static":false}
        }
    }]],
    ["dexprobe.Probe"] = [[{
        "name":"dexprobe.Probe",
        "constructors":[{"sig":"()V","params":[]}],
        "methods":{
            "add":[{"sig":"(II)I","static":true,"params":["I","I"],"ret":"I"}]
        },
        "fields":{}
    }]],
    ["java.util.ArrayList"] = [[{
        "name":"java.util.ArrayList",
        "constructors":[{"sig":"()V","params":[]}],
        "methods":{
            "iterator":[{"sig":"()Ljava/util/Iterator;","static":false,"params":[],"ret":"Ljava/util/Iterator;"}]
        },
        "fields":{}
    }]],
    ["java.util.HashMap"] = [[{
        "name":"java.util.HashMap",
        "constructors":[{"sig":"()V","params":[]}],
        "methods":{
            "entrySet":[{"sig":"()Ljava/util/Set;","static":false,"params":[],"ret":"Ljava/util/Set;"}]
        },
        "fields":{}
    }]],
    ["android.app.Activity"] = [[{
        "name":"android.app.Activity",
        "constructors":[{"sig":"()V","params":[]}],
        "methods":{
            "onCreate":[{"sig":"(Landroid/os/Bundle;)V","static":false,"params":["Landroid/os/Bundle;"],"ret":"V"}]
        },
        "fields":{}
    }]],
    ["com.demo.ClassArgCtor"] = [[{
        "name":"com.demo.ClassArgCtor",
        "constructors":[
            {"sig":"()V","params":[]},
            {"sig":"(Ljava/lang/Class;)V","params":["Ljava/lang/Class;"]}
        ],
        "methods":{},
        "fields":{}
    }]],
    ["com.demo.OverloadEdge"] = [[{
        "name":"com.demo.OverloadEdge",
        "constructors":[{"sig":"()V","params":[]}],
        "methods":{
            "baz":[
                {"sig":"(I)Ljava/lang/String;","static":false,"params":["I"],"ret":"Ljava/lang/String;"},
                {"sig":"(Lcom/demo/OverloadEdge;Ljava/lang/String;)Ljava/lang/String;","static":false,"params":["Lcom/demo/OverloadEdge;","Ljava/lang/String;"],"ret":"Ljava/lang/String;"}
            ],
            "num":[
                {"sig":"(F)Ljava/lang/String;","static":false,"params":["F"],"ret":"Ljava/lang/String;"},
                {"sig":"(D)Ljava/lang/String;","static":false,"params":["D"],"ret":"Ljava/lang/String;"}
            ],
            "obf":[
                {"sig":"(I)Ljava/lang/String;","static":false,"params":["I"],"ret":"Ljava/lang/String;"},
                {"sig":"(LI;)Ljava/lang/String;","static":false,"params":["LI;"],"ret":"Ljava/lang/String;"}
            ]
        },
        "fields":{}
    }]],
    ["I"] = [[{
        "name":"I",
        "constructors":[{"sig":"()V","params":[]}],
        "methods":{},
        "fields":{}
    }]],
}

local wakelock_java_release_calls = 0
local installed_hooks = {}
local live_iterators = 0

java = {
    invoke = function(target, method, sig, ...)
        local args = table.pack(...)
        last_call = { kind = "invoke", target = target, method = method, sig = sig, args = args }
        if method == "returnNull" then return nil
        elseif method == "returnString" then return "hello"
        elseif method == "returnInt" then return 42
        elseif method == "returnObject" then return alloc_handle("java.lang.Object")
        elseif method == "returnArray" then return alloc_handle("[B", { bytes = "xyz" })
        elseif method == "currentApplication" then return alloc_handle("android.app.Application")
        elseif method == "getPackageManager" then return alloc_handle("android.content.pm.PackageManager")
        elseif method == "getPackageName" then return "com.example.demo"
        elseif method == "getLaunchIntentForPackage" then return alloc_handle("android.content.Intent", { action = "MAIN:" .. args[1] })
        elseif method == "getAction" then return obj_data[target].action or "DEFAULT"
        elseif method == "append" then
            obj_data[target].buf = (obj_data[target].buf or "") .. tostring(args[1])
            return alloc_handle("java.lang.StringBuilder", { buf = obj_data[target].buf })
        elseif method == "toString" then
            local d = obj_data[target]
            return d.str or d.buf or ("obj@" .. tostring(target))
        elseif method == "booleanValue" then return obj_data[target].val
        elseif method == "longValue" then return obj_data[target].val
        elseif method == "doubleValue" then return obj_data[target].val
        elseif method == "iterator" then
            live_iterators = live_iterators + 1
            return alloc_handle("java.util.Iterator", {
                items = obj_data[target].items,
                count = obj_data[target].count,
                pos = 0,
                is_iter = true,
            })
        elseif method == "entrySet" then
            live_iterators = live_iterators + 1
            return alloc_handle("java.util.Set", { items = obj_data[target].entries, is_iter = true })
        elseif method == "hasNext" then
            local d = obj_data[target]
            local count = d.count or #d.items
            return d.pos < count
        elseif method == "next" then
            local d = obj_data[target]
            d.pos = d.pos + 1
            local item = d.items[d.pos]
            if item == nil or item.is_null then
                return nil
            end
            return alloc_handle(item.cls, item.data)
        elseif method == "getKey" then
            local k = obj_data[target].k
            if k == nil then return nil end
            return alloc_handle(k.cls, k.data)
        elseif method == "getValue" then
            local v = obj_data[target].v
            if v == nil then return nil end
            return alloc_handle(v.cls, v.data)
        elseif method == "valueOf" then return sig .. ":" .. tostring(args[1])
        elseif method == "release" then
            wakelock_java_release_calls = wakelock_java_release_calls + 1
            return nil
        elseif method == "equals" then return target == args[1]
        elseif method == "handle" then return "handled:" .. sig
        elseif method == "ambig" then return "ambig:" .. sig
        elseif method == "baz" then return "baz:" .. sig
        elseif method == "num" then return "num:" .. sig
        elseif method == "obf" then return "obf:" .. sig
        elseif method == "add" then return args[1] + args[2]
        end
        return nil
    end,
    _invoke_as = function(handle, cls, method, sig, ...)
        last_call = { kind = "invoke_as", target = handle, cls = cls, method = method, sig = sig }
        if method == "secret" then return "base-secret-from:" .. cls end
        return nil
    end,
    new = function(cls, sig, ...)
        local args = table.pack(...)
        last_call = { kind = "new", cls = cls, sig = sig, args = args }
        local clean_cls = cls:match("^%d+:(.+)$") or cls
        if clean_cls == "java.lang.StringBuilder" then
            local init = sig == "(Ljava/lang/String;)V" and args[1] or ""
            return alloc_handle(clean_cls, { buf = init, cap = sig == "(I)V" and args[1] or 16 })
        elseif clean_cls == "android.content.Intent" then
            return alloc_handle(clean_cls, { ctx = args[1], target_cls = args[2], action = "EXPLICIT" })
        elseif clean_cls == "com.demo.ClassArgCtor" then
            return alloc_handle(clean_cls, { ctor_sig = sig, cls_arg = args[1] })
        end
        return alloc_handle(clean_cls, {})
    end,
    load_dex = function(path)
        assert(type(path) == "string")
        return alloc_handle("dalvik.system.InMemoryDexClassLoader", { path = path })
    end,
    release = function(id)
        assert(type(id) == "number" and math.type(id) == "integer" and id > 0,
               "raw release expects positive integer")
        assert(live[id], "unknown Java object handle: " .. tostring(id))
        if obj_data[id] and obj_data[id].is_iter then
            live_iterators = live_iterators - 1
        end
        live[id] = nil
        release_counts[id] = (release_counts[id] or 0) + 1
        return true
    end,
    _reflect = function(target)
        local key = type(target) == "number" and obj_class[target] or (target:match("^%d+:(.+)$") or target)
        local meta = REFLECT_META[key]
        assert(meta, "unknown reflected class: " .. tostring(key))
        return meta
    end,
    _class_name = function(id)
        assert(live[id], "dead handle in _class_name")
        return obj_class[id]
    end,
    _class_object = function(target)
        local clean = target:match("^%d+:(.+)$") or target
        return alloc_handle("java.lang.Class", { class_for = clean })
    end,
    _is_instance = function(id, target)
        assert(live[id], "dead handle in _is_instance")
        local c = obj_class[id]
        local t = target:match("^%d+:(.+)$") or target
        if c == t or t == "java.lang.Object" then return true end
        if c == "com.demo.Sub" and t == "com.demo.Base" then return true end
        if c == "android.app.Application" and t == "android.content.Context" then return true end
        if c == "java.util.ArrayList" and t == "java.lang.Iterable" then return true end
        if c == "java.util.HashMap" and t == "java.util.Map" then return true end
        if c == "java.lang.StringBuilder" and t == "java.lang.CharSequence" then return true end
        return false
    end,
    _get_field = function(target, name, sig, decl)
        if type(target) == "string" then
            return static_fields[target .. "." .. name]
        end
        local key = tostring(target) .. ":" .. (decl or obj_class[target]) .. "." .. name
        return instance_fields[key]
    end,
    _set_field = function(target, name, sig, value, decl)
        if type(target) == "string" then
            static_fields[target .. "." .. name] = value
            return true
        end
        local key = tostring(target) .. ":" .. (decl or obj_class[target]) .. "." .. name
        instance_fields[key] = value
        return true
    end,
    _array_new = function(elem_sig, len_or_bytes)
        if elem_sig == "B" and type(len_or_bytes) == "string" then
            local arr = {}
            for i = 1, #len_or_bytes do
                local b = len_or_bytes:byte(i)
                arr[i] = b >= 128 and (b - 256) or b
            end
            return alloc_handle("[B", { array = arr, bytes = len_or_bytes })
        end
        local arr = {}
        local is_obj = elem_sig:sub(1, 1) == "L" or elem_sig:sub(1, 1) == "["
        if not is_obj then
            for i = 1, len_or_bytes do arr[i] = 0 end
        end
        return alloc_handle("[" .. elem_sig:gsub("/", "."), { array = arr, len = len_or_bytes })
    end,
    _array_length = function(id)
        assert(live[id])
        local d = obj_data[id]
        return d.len or #d.array
    end,
    _array_get = function(id, elem_sig, idx)
        assert(live[id])
        local d = obj_data[id]
        assert(idx >= 0 and idx < (d.len or #d.array))
        local val = d.array[idx + 1]
        if (elem_sig:sub(1, 1) == "L" or elem_sig:sub(1, 1) == "[") and elem_sig ~= "Ljava/lang/String;" and type(val) == "number" then
            -- Mirror real JNI: GetObjectArrayElement registers a fresh GlobalRef handle.
            return alloc_handle(obj_class[val], obj_data[val])
        end
        return val
    end,
    _array_set = function(id, elem_sig, idx, val)
        assert(live[id])
        local d = obj_data[id]
        assert(idx >= 0 and idx < (d.len or #d.array))
        d.array[idx + 1] = val
        if elem_sig == "B" then
            local chars = {}
            for i = 1, #d.array do
                local b = d.array[i]
                if b < 0 then b = b + 256 end
                chars[i] = string.char(b)
            end
            d.bytes = table.concat(chars)
        end
        return true
    end,
    _array_bytes = function(id)
        assert(live[id])
        return obj_data[id].bytes
    end,
}

local removed_hooks = {}
hook = {
    install = function(cls, method, sig, src)
        local id = #installed_hooks + 100
        installed_hooks[#installed_hooks + 1] = { id = id, cls = cls, method = method, sig = sig, src = src }
        return id
    end,
    remove = function(id) removed_hooks[#removed_hooks + 1] = id; return true end,
    stream_release = function(lease) removed_hooks[#removed_hooks + 1] = "stream:" .. tostring(lease); return true end,
}
local removed_native_hooks = {}
native = {
    uninstall = function(id) removed_native_hooks[#removed_native_hooks + 1] = id; return true end,
}

-- Load resources.lua twice to verify idempotence.
assert(loadfile(root .. "/config/resources.lua"))()
assert(loadfile(root .. "/config/resources.lua"))()

-- 1) Primitive, string, and nil returns stay raw; objects/arrays become managed proxies.
assert(java.invoke("C", "returnInt", "()I") == 42)
assert(not java.is_ref(42))
assert(java.invoke("C", "returnString", "()Ljava/lang/String;") == "hello")
assert(java.invoke("C", "returnNull", "()Ljava/lang/Object;") == nil)

-- 2) Lost reference triggers __gc automatic release.
local gc_id
do
    local tmp = java.new("java.lang.StringBuilder", "()V")
    assert(java.is_ref(tmp) and tmp:__is_alive() and java.is_alive(tmp))
    gc_id = tmp:__id()
    assert(gc_id == java.id(tmp) and live[gc_id] == true)
    tmp = nil
end
collectgarbage("collect")
collectgarbage("collect")
assert(live[gc_id] == nil and release_counts[gc_id] == 1)

-- 3) Lua 5.4 <close> releases immediately on normal exit AND on error.
local close_id
do
    local h <close> = java.new("java.lang.Object", "()V")
    close_id = h:__id()
    assert(live[close_id] == true)
end
assert(live[close_id] == nil)
collectgarbage("collect")
assert(release_counts[close_id] == 1)

local err_close_id
local ok_err = pcall(function()
    local h <close> = java.new("java.lang.Object", "()V")
    err_close_id = h:__id()
    error("boom")
end)
assert(not ok_err and live[err_close_id] == nil and release_counts[err_close_id] == 1)

-- 4) Manual release, raw-id release, and detach all prevent double-release.
local man_id, raw_id, detached_id
do
    local a = java.new("java.lang.Object", "()V")
    man_id = a:__id()
    assert(java.release(a) == true and not a:__is_alive() and a:__id() == nil)
    assert(a:__release() == true)
    assert(not pcall(function() java.invoke(a, "returnInt", "()I") end))

    local b = java.new("java.lang.Object", "()V")
    raw_id = b:__id()
    assert(java.release(raw_id) == true and not b:__is_alive())

    local loader = java.load_dex("/tmp/test.dex")
    local lid = loader:__id()
    assert(loader .. ":dexprobe.Probe" == tostring(lid) .. ":dexprobe.Probe")
    detached_id = java.detach(loader)
    assert(detached_id == lid and not loader:__is_alive())
end
collectgarbage("collect")
collectgarbage("collect")
assert(release_counts[man_id] == 1 and release_counts[raw_id] == 1)
assert(live[detached_id] == true)
assert(java.release(detached_id) == true and live[detached_id] == nil)

-- 5) Frida-style java.use() + direct obj:method(...) + chaining + GC of intermediates!
local ActivityThread = java.use("android.app.ActivityThread")
assert(java.is_class(ActivityThread) and ActivityThread.__name == "android.app.ActivityThread")
local app <close> = ActivityThread.currentApplication()
local app2 <close> = ActivityThread:currentApplication()
assert(app:getPackageName() == "com.example.demo")
assert(app2:getPackageName() == "com.example.demo")
local intent <close> = app:getPackageManager():getLaunchIntentForPackage("com.example.demo")
assert(intent:getAction() == "MAIN:com.example.demo")

-- 6) Constructors (__new & :new) + Proxy arguments (instance proxy + Class<?> class proxy!).
local Intent = java.use("android.content.Intent")
local MainActivity = java.use("com.example.MainActivity")
local explicit <close> = Intent:__new(app, MainActivity)
assert(explicit:getAction() == "EXPLICIT")
assert(obj_data[explicit:__id()].ctx == app:__id())
local cls_arg_id = obj_data[explicit:__id()].target_cls
assert(obj_class[cls_arg_id] == "java.lang.Class" and obj_data[cls_arg_id].class_for == "com.example.MainActivity")

local StringBuilder = java.use("java.lang.StringBuilder")
local sb <close> = StringBuilder:new("hello")
local sb2 <close> = sb:append(" world")
assert(sb2:toString() == "hello world")
local sb_cap <close> = StringBuilder["__new"]:overload("int")(64)
assert(obj_data[sb_cap:__id()].cap == 64)

-- 7) Overload scoring + explicit :overload(...) + ambiguity error.
local String = java.use("java.lang.String")
assert(String.valueOf(42) == "(I)Ljava/lang/String;:42")
assert(String.valueOf(5000000000) == "(J)Ljava/lang/String;:5000000000")
assert(String.valueOf(3.5) == "(D)Ljava/lang/String;:3.5")
assert(String.valueOf(true) == "(Z)Ljava/lang/String;:true")
assert(String.valueOf:overload("long")(42) == "(J)Ljava/lang/String;:42")
assert(String.valueOf:overload("java.lang.Object")(app) == "(Ljava/lang/Object;)Ljava/lang/String;:" .. tostring(app:__id()))

local Sub = java.use("com.demo.Sub")
local sub <close> = Sub:__new()
-- Sub is an instance of Base (score 3) vs Object (score 2) -> picks handle(Base)!
assert(sub:handle(sub) == "handled:(Lcom/demo/Base;)Ljava/lang/String;")
-- Passing unrelated instance (app) -> picks handle(Object)!
assert(sub:handle(app) == "handled:(Ljava/lang/Object;)Ljava/lang/String;")
-- Passing nil when both String and File match -> clear ambiguity error!
local ok_ambig, err_ambig = pcall(function() sub:ambig(nil) end)
assert(not ok_ambig and err_ambig:find("ambiguous overload", 1, true))
-- Explicit :overload resolves nil cleanly (both dot and colon style)!
assert(sub.ambig:overload("java.io.File")(nil) == "ambig:(Ljava/io/File;)Ljava/lang/String;")
assert(sub.ambig:overload("java.lang.String")(sub, nil) == "ambig:(Ljava/lang/String;)Ljava/lang/String;")

-- 8) Method name collision safety: WakeLock:release() calls Java release(), NOT JNI release!
local WakeLock = java.use("com.demo.WakeLock")
local wl <close> = WakeLock:__new()
wl:release()
assert(wakelock_java_release_calls == 1 and wl:__is_alive())
-- Self-argument colon vs dot: equals(self) works in BOTH forms!
assert(wl:equals(wl) == true)
assert(wl.equals(wl) == true)
assert(wl:equals(app) == false)

-- 9) Static & instance fields + __cast view (shadowed fields, superclass call, no double-release).
local Build = java.use("android.os.Build")
assert(Build.MODEL == "Pixel 9" and Build.DEBUG == false)
Build.DEBUG = true
assert(Build.DEBUG == true and Build:__field("MODEL") == "Pixel 9")

sub.count = 77
sub.tag = "sub-tag"
assert(sub.count == 77 and sub.tag == "sub-tag")
local base_view = sub:__cast("com.demo.Base")
base_view:__set_field("tag", "base-tag")
assert(base_view.tag == "base-tag" and sub.tag == "sub-tag")
assert(base_view:secret() == "base-secret-from:com.demo.Base")
-- Dropping cast view does NOT release sub!
base_view = nil
collectgarbage("collect")
collectgarbage("collect")
assert(sub:__is_alive() and live[sub:__id()] == true)

-- 10) Java arrays & binary-safe byte[].
local raw_bytes <close> = java.bytes("a\0b\0c")
assert(#raw_bytes == 5 and raw_bytes:__length() == 5)
assert(raw_bytes:__bytes() == "a\0b\0c")
raw_bytes:__set(0, 65)
assert(raw_bytes:__get(0) == 65 and raw_bytes:__bytes() == "A\0b\0c")

local str_arr <close> = java.array("java.lang.String", { "x", "y" })
assert(#str_arr == 2 and str_arr:__get(0) == "x" and str_arr:__get(1) == "y")
local tbl = str_arr:__to_table()
assert(#tbl == 2 and tbl[1] == "x" and tbl[2] == "y")

-- Object array accepts proxy elements and returns fresh GC-managed proxy elements.
local obj_arr <close> = java.array("com.demo.Sub", { sub })
assert(#obj_arr == 1)
assert(obj_data[obj_arr:__id()].array[1] == sub:__id())
local elem <close> = obj_arr:__get(0)
assert(elem:__class_name() == "com.demo.Sub" and elem:__id() ~= sub:__id())

-- 11) DexClassLoader kept alive by java.use(..., loader)!
local loader_id
do
    local Probe
    do
        local loader = java.load_dex("/tmp/probe.dex")
        loader_id = loader:__id()
        Probe = java.use("dexprobe.Probe", loader)
    end
    collectgarbage("collect")
    collectgarbage("collect")
    assert(live[loader_id] == true, "Class proxy must keep its DexClassLoader alive")
    assert(Probe.add(3, 4) == 7)
    assert(Probe:add(10, 20) == 30)
end
collectgarbage("collect")
collectgarbage("collect")
assert(live[loader_id] == nil, "DexClassLoader should GC once class proxy is dropped")

-- 12) java.scope(fn) LIFO release + nested scope:keep() + nil-hole returns.
local s1, s2, kept_ref
local r1, r2, r3 = java.scope(function(outer)
    local a = StringBuilder:__new("outer")
    s1 = a:__id()
    kept_ref = java.scope(function(inner)
        local b = java.bytes("tmp")
        local c = StringBuilder:__new("kept")
        s2 = b:__id()
        return inner:keep(c)
    end)
    assert(live[s2] == nil and kept_ref:__is_alive())
    outer:keep(kept_ref)
    return "ok", nil, 99
end)
assert(r1 == "ok" and r2 == nil and r3 == 99)
assert(live[s1] == nil and kept_ref:__is_alive())
local kept_id = kept_ref:__id()
kept_ref = nil
collectgarbage("collect")
collectgarbage("collect")
assert(live[kept_id] == nil)

-- 13) dejavu.scope(): LIFO cleanup + retryable busy RPC + <close>.
local events = {}
local rpc_busy = true
local fake_rpc = {
    unregister = function()
        events[#events + 1] = "rpc"
        if rpc_busy then return false, "busy" end
        return true
    end,
}
local jref = java.new("java.lang.Object", "()V")
local jref_id = jref:__id()

local sc = dejavu.scope()
sc:track_rpc(fake_rpc)
sc:track_hook(11)
sc:track_native_hook(22)
sc:track_stream("lease-1")
sc:track_java(jref)
sc:defer(function() events[#events + 1] = "defer"; return true end)
assert(sc:count() == 6)

local closed, reason = sc:close()
assert(closed == false and reason == "busy" and sc:count() == 1)
assert(live[jref_id] == nil)
assert(removed_hooks[1] == "stream:lease-1" and removed_hooks[2] == 11)
assert(removed_native_hooks[1] == 22)
assert(events[1] == "defer" and events[2] == "rpc")

rpc_busy = false
closed, reason = sc:close()
assert(closed == true and reason == nil and sc:count() == 0)
assert(#events == 3 and events[3] == "rpc")

local defer_ran = false
do
    local s <close> = dejavu.scope()
    s:defer(function() defer_ran = true end)
end
assert(defer_ran == true)

-- 14) Direct class __call construction: Cls(...)
do
    local sb_direct <close> = StringBuilder("direct")
    assert(sb_direct:toString() == "direct")
    local i_direct <close> = Intent(app, MainActivity)
    assert(i_direct:getAction() == "EXPLICIT")
end

-- 15) 1-based Lua array indexing & built-in ipairs(arr)
do
    local arr <close> = java.array("java.lang.String", { "alpha", "beta" })
    assert(arr[1] == "alpha" and arr[2] == "beta" and arr[3] == nil and arr[0] == nil)
    arr[2] = "BETA"
    assert(arr[2] == "BETA")
    local seen = {}
    for i, v in ipairs(arr) do
        seen[i] = v
    end
    assert(#seen == 2 and seen[1] == "alpha" and seen[2] == "BETA")
end

-- 16) Explicit typed primitive markers & typed java.null(...)
assert(String.valueOf(java.long(42)) == "(J)Ljava/lang/String;:42")
assert(String.valueOf(java.double(7)) == "(D)Ljava/lang/String;:7")
assert(String.valueOf(java.boolean(false)) == "(Z)Ljava/lang/String;:false")
assert(sub:ambig(java.null("java.io.File")) == "ambig:(Ljava/io/File;)Ljava/lang/String;")
assert(sub:ambig(java.null("java.lang.String")) == "ambig:(Ljava/lang/String;)Ljava/lang/String;")

-- 17) __instanceof, __str, __inspect, and java.app()
local Base = java.use("com.demo.Base")
assert(sub:__instanceof(Base) == true)
assert(sub:__instanceof("com.demo.Base") == true)
assert(sub:__instanceof(WakeLock) == false)
assert(java.instanceof(nil, Base) == false)
assert(sb:__str() == "hello world" and java.str(sb) == "hello world")
local insp = StringBuilder:__inspect()
assert(insp:find("class java.lang.StringBuilder", 1, true))
assert(insp:find("append(java.lang.String)", 1, true))
local insp_filtered = StringBuilder:__inspect("toString")
assert(insp_filtered:find("toString()", 1, true) and not insp_filtered:find("append", 1, true))
assert(sub:__inspect("tag"):find("field java.lang.String tag", 1, true))
do
    local a <close> = java.app()
    assert(a:getPackageName() == "com.example.demo")
end

-- 18) java.each() with Lua 5.4 generic-for <close> (including early break & nulls!) + java.to_lua()
do
    local raw_list_id = alloc_handle("java.util.ArrayList", {
        count = 4,
        items = {
            { cls = "java.lang.String", data = { str = "one" } },
            { is_null = true },
            { cls = "java.lang.Integer", data = { val = 2 } },
            { cls = "java.lang.Boolean", data = { val = false } },
        },
    })
    local list <close> = java.own(raw_list_id)
    local collected = {}
    local seen_count = 0
    for idx, item in java.each(list) do
        seen_count = seen_count + 1
        collected[idx] = item == nil and "NIL" or java.to_lua(item)
        if idx == 3 then
            break -- Early break must still deterministically close Iterator!
        end
    end
    assert(live_iterators == 0, "Iterator leaked after early break in java.each(list)")
    assert(seen_count == 3 and collected[1] == "one" and collected[2] == "NIL" and collected[3] == 2)

    -- Array with null element must also not truncate in java.each!
    local nullable_arr <close> = java.array("java.lang.String", 3)
    nullable_arr[1] = "first"
    nullable_arr[3] = "third"
    local arr_seen = {}
    for idx, item in java.each(nullable_arr) do
        arr_seen[idx] = item == nil and "NIL" or item
    end
    assert(arr_seen[1] == "first" and arr_seen[2] == "NIL" and arr_seen[3] == "third")

    -- java.to_lua preserves false booleans, maps nulls to json.null, and works even via __cast("java.lang.Object")!
    local obj_view = list:__cast("java.lang.Object")
    local lua_list = java.to_lua(obj_view)
    assert(#lua_list == 4 and lua_list[1] == "one" and lua_list[2] == JSON_NULL and
           lua_list[3] == 2 and lua_list[4] == false)
    assert(live_iterators == 0)

    local lua_arr = java.to_lua(nullable_arr)
    assert(#lua_arr == 3 and lua_arr[1] == "first" and lua_arr[2] == JSON_NULL and lua_arr[3] == "third")

    local raw_map_id = alloc_handle("java.util.HashMap", {
        entries = {
            {
                cls = "java.util.Map$Entry",
                data = {
                    k = nil, -- null key must yield java.NULL in java.each and "null" in java.to_lua!
                    v = { cls = "java.lang.String", data = { str = "from-null-key" } },
                },
            },
            {
                cls = "java.util.Map$Entry",
                data = {
                    k = { cls = "java.lang.String", data = { str = "nums" } },
                    v = {
                        cls = "java.util.ArrayList",
                        data = {
                            items = {
                                { cls = "java.lang.Double", data = { val = 1.5 } },
                                { cls = "[B", data = { bytes = "\xde\xad\xbe\xef" } },
                            },
                        },
                    },
                },
            },
            {
                cls = "java.util.Map$Entry",
                data = {
                    k = { cls = "java.lang.String", data = { str = "empty" } },
                    v = nil,
                },
            },
        },
    })
    local map <close> = java.own(raw_map_id)
    local map_keys = {}
    for k, v in java.each(map) do
        map_keys[#map_keys + 1] = k == java.NULL and "<NULL>" or java.to_lua(k)
        if #map_keys == 2 then
            break -- Early break on Map releases BOTH EntrySet and Iterator!
        end
    end
    assert(live_iterators == 0, "Map EntrySet/Iterator leaked after early break")
    assert(map_keys[1] == "<NULL>" and map_keys[2] == "nums")

    local lua_map_raw = java.to_lua(map)
    assert(lua_map_raw["null"] == "from-null-key" and lua_map_raw.empty == JSON_NULL and
           lua_map_raw.nums[1] == 1.5 and lua_map_raw.nums[2] == "\xde\xad\xbe\xef")
    local lua_map_hex = java.to_lua(map, { bytes = "hex" })
    assert(lua_map_hex.nums[2] == "deadbeef")
    assert(live_iterators == 0)
end

-- 19) Method proxy :hook(c_source), instance method on Cls, ctor & custom-loader rejection
do
    local hid1 = ActivityThread.currentApplication:hook("int before_hook(dejavu_hook_context *c){return 0;}")
    assert(hid1 == 100 and installed_hooks[1].cls == "android.app.ActivityThread" and
           installed_hooks[1].method == "currentApplication" and
           installed_hooks[1].sig == "()Landroid/app/Application;")
    -- Multi-overload method rejects :hook() without :overload(...).
    assert(not pcall(function() String.valueOf:hook("src") end))
    local ov = String.valueOf:overload("long")
    assert(ov.sig == "(J)Ljava/lang/String;" and ov:signature() == "(J)Ljava/lang/String;")
    local hid2 = ov:hook("int after_hook(dejavu_hook_context *c){return 0;}")
    assert(hid2 == 101 and installed_hooks[2].sig == "(J)Ljava/lang/String;")

    -- Instance method proxy obtained from java.use(Cls) supports :hook(), while direct call fails clearly.
    local Activity = java.use("android.app.Activity")
    local hid3 = Activity.onCreate:hook("int before_hook(dejavu_hook_context *c){return 0;}")
    assert(hid3 == 102 and installed_hooks[3].cls == "android.app.Activity" and
           installed_hooks[3].method == "onCreate" and
           installed_hooks[3].sig == "(Landroid/os/Bundle;)V")
    local ok_inst, err_inst = pcall(function() Activity.onCreate(nil) end)
    assert(not ok_inst and err_inst:find("requires a Java object receiver", 1, true))

    -- Constructors (<init>) are rejected by :hook() (LSPlant only hooks methods).
    local ok_ctor_hook, err_ctor_hook = pcall(function()
        StringBuilder.__new:overload("java.lang.String"):hook("src")
    end)
    assert(not ok_ctor_hook and err_ctor_hook:find("does not support constructors", 1, true))

    -- Custom DexClassLoader classes are rejected by :hook().
    local dex_loader <close> = java.load_dex("/tmp/hook_test.dex")
    local DexProbe = java.use("dexprobe.Probe", dex_loader)
    local ok_dex_hook, err_dex_hook = pcall(function() DexProbe.add:hook("src") end)
    assert(not ok_dex_hook and err_dex_hook:find("app ClassLoader", 1, true))
end

-- 20) Cls(Cls) constructor & type-aware strip_bound_self & int -> double > float & 1-letter R8 class "I"
do
    local ClassArgCtor = java.use("com.demo.ClassArgCtor")
    local inst <close> = ClassArgCtor(ClassArgCtor)
    assert(obj_data[inst:__id()].ctor_sig == "(Ljava/lang/Class;)V",
           "Cls(Cls) must pass Cls as Class<?> rather than stripping self")
    assert(obj_data[obj_data[inst:__id()].cls_arg].class_for == "com.demo.ClassArgCtor")

    local Edge = java.use("com.demo.OverloadEdge")
    local e <close> = Edge()
    -- baz(int) vs baz(OverloadEdge, String): passing (e, "hi") via dot syntax must NOT strip e!
    assert(e.baz(e, "hi") == "baz:(Lcom/demo/OverloadEdge;Ljava/lang/String;)Ljava/lang/String;")
    assert(e:baz(123) == "baz:(I)Ljava/lang/String;")
    -- num(float) vs num(double): integer prefers double without ambiguity!
    assert(e:num(7) == "num:(D)Ljava/lang/String;")
    -- 1-letter R8 class "I" is treated as LI;, NOT primitive int!
    local ObfI = java.use("I")
    local obf_obj <close> = ObfI()
    assert(e:obf(obf_obj) == "obf:(LI;)Ljava/lang/String;")
    assert(e:obf(42) == "obf:(I)Ljava/lang/String;")
end

-- 21) Validated __cast (string & class proxy), base_view:__invoke, and friendly array __instanceof
do
    -- Invalid cast rejected immediately!
    local ok_bad_cast, err_bad_cast = pcall(function() sub:__cast(WakeLock) end)
    assert(not ok_bad_cast and err_bad_cast:find("is not an instance of", 1, true))
    -- Valid cast with class proxy + base_view:__invoke routes via _invoke_as!
    local base_v = sub:__cast(Base)
    assert(base_v:__invoke("secret", "()Ljava/lang/String;") == "base-secret-from:com.demo.Base")
    -- Cast view __instanceof still reflects runtime class!
    local obj_v = sub:__cast("java.lang.Object")
    assert(obj_v:__instanceof(Base) == true and obj_v:__instanceof(Sub) == true)
    -- Friendly array type names in __instanceof!
    local b_arr <close> = java.bytes("abc")
    assert(b_arr:__instanceof("byte[]") == true and b_arr:__instanceof("[B") == true)
    local s_arr <close> = java.array("java.lang.String", 1)
    assert(s_arr:__instanceof("java.lang.String[]") == true)
end

-- 22) java.invoke(Cls, ...) static dispatch + java.use("<loader_id>:<class>")
do
    assert(java.invoke(String, "valueOf", "(I)Ljava/lang/String;", 99) == "(I)Ljava/lang/String;:99")
    local loader <close> = java.load_dex("/tmp/str_loader.dex")
    local Probe2 = java.use(loader .. ":dexprobe.Probe")
    assert(Probe2.add(5, 6) == 11)
end

-- 23) java.scope:keep(cast_view) + nested outer:keep() inside inner scope
do
    local kept_cast, kept_outer
    local inner_dead_id
    java.scope(function(outer)
        local view = java.scope(function(inner)
            local s = Sub()
            local dead = Sub()
            inner_dead_id = dead:__id()
            -- Keep a cast view: must keep the underlying root ref!
            local v = s:__cast(Base)
            outer:keep(s) -- Calling outer:keep directly from inside inner!
            return inner:keep(v)
        end)
        assert(live[inner_dead_id] == nil and view:__is_alive())
        kept_cast = outer:keep(view)
        kept_outer = view
    end)
    assert(kept_cast:__is_alive() and live[kept_cast:__id()] == true)
    local id_to_gc = kept_cast:__id()
    kept_cast = nil
    kept_outer = nil
    collectgarbage("collect")
    collectgarbage("collect")
    assert(live[id_to_gc] == nil)
end

-- 24) dejavu.scope():track_java validation & idempotent cleanup on already-released raw int handle
do
    local sc2 = dejavu.scope()
    assert(not pcall(function() sc2:track_java("not-a-handle") end))
    assert(not pcall(function() sc2:track_java(0) end))
    local raw_h = java.detach(java.new("java.lang.Object", "()V"))
    sc2:track_java(raw_h)
    -- Manually release raw_h before scope closes; scope:close() must NOT wedge on "unknown Java object handle"!
    assert(java.release(raw_h) == true)
    local ok_close, close_err = sc2:close()
    assert(ok_close == true and close_err == nil and sc2:count() == 0)
end

print("OK: Lua Java proxy (use/methods/fields/overloads/cast/arrays/sugar/review-fixes), GC/<close>/scope and dejavu.scope passed")
