-- Host regression tests for zygisk/config/resources.lua:
--   1) Automatic GC (__gc) release of Java GlobalRef handles on lost reference
--   2) Deterministic Lua 5.4 <close> (__close) release (normal exit + error)
--   3) Idempotent manual java.release / ref:release / raw-id release (no double-release)
--   4) ref:detach(), loader .. ":Class" (__concat), ref:invoke(), use-after-release error
--   5) Nested java.scope(fn) LIFO cleanup + scope:keep(ref) + multi-return with nil holes
--   6) dejavu.scope() LIFO cleanup for RPC/hook/native_hook/stream/java/defer + retryable busy

local root = assert(arg[1], "usage: lua resources_test.lua <zygisk-dir>")

local next_id = 0
local live = {}
local released = {}
local release_counts = {}
local last_invoke_args = nil

local function alloc_handle()
    next_id = next_id + 1
    live[next_id] = true
    return next_id
end

java = {
    invoke = function(target, method, sig, ...)
        last_invoke_args = table.pack(target, method, sig, ...)
        if method == "returnNull" then
            return nil
        elseif method == "returnString" then
            return "hello"
        elseif method == "returnInt" then
            return 42
        elseif method == "returnObject" or method == "returnArray" then
            return alloc_handle()
        end
        return nil
    end,
    new = function(cls, sig, ...)
        last_invoke_args = table.pack(cls, sig, ...)
        return alloc_handle()
    end,
    load_dex = function(path)
        assert(type(path) == "string")
        return alloc_handle()
    end,
    release = function(id)
        assert(type(id) == "number" and math.type(id) == "integer" and id > 0,
               "raw release expects positive integer")
        assert(live[id], "double release or unknown handle: " .. tostring(id))
        live[id] = nil
        released[#released + 1] = id
        release_counts[id] = (release_counts[id] or 0) + 1
        return true
    end,
}

local removed_hooks = {}
hook = {
    remove = function(id)
        removed_hooks[#removed_hooks + 1] = id
        return true
    end,
    stream_release = function(lease)
        removed_hooks[#removed_hooks + 1] = "stream:" .. tostring(lease)
        return true
    end,
}

local removed_native_hooks = {}
native = {
    uninstall = function(id)
        removed_native_hooks[#removed_native_hooks + 1] = id
        return true
    end,
}

-- Load resources.lua twice to also verify idempotent bootstrap.
assert(loadfile(root .. "/config/resources.lua"))()
assert(loadfile(root .. "/config/resources.lua"))()

-- 1) Primitive, string, and nil returns are NOT wrapped; objects and arrays ARE wrapped.
assert(java.invoke("C", "returnInt", "()I") == 42)
assert(not java.is_ref(42))
assert(java.invoke("C", "returnString", "()Ljava/lang/String;") == "hello")
assert(java.invoke("C", "returnNull", "()Ljava/lang/Object;") == nil)

-- 2) Lost reference triggers __gc automatic release!
local gc_id
do
    local tmp = java.new("java.lang.StringBuilder", "()V")
    assert(java.is_ref(tmp) and tmp:is_alive())
    gc_id = tmp:id()
    assert(live[gc_id] == true)
    tmp = nil
end
collectgarbage("collect")
collectgarbage("collect")
assert(live[gc_id] == nil, "dropped java_ref was not released by GC")
assert(release_counts[gc_id] == 1, "expected single GC release")

-- 3) Object/array returns from java.invoke also GC-release on lost reference.
local obj_id, arr_id
do
    local o = java.invoke("C", "returnObject", "()Ljava/lang/Object;")
    local a = java.invoke("C", "returnArray", "()[B")
    assert(java.is_ref(o) and java.is_ref(a))
    obj_id, arr_id = o:id(), a:id()
end
collectgarbage("collect")
collectgarbage("collect")
assert(live[obj_id] == nil and live[arr_id] == nil)

-- 4) Lua 5.4 <close> releases immediately on normal exit AND on error, without double-release on GC.
local close_id
do
    local h <close> = java.new("java.lang.Object", "()V")
    close_id = h:id()
    assert(live[close_id] == true)
end
assert(live[close_id] == nil, "<close> did not release immediately at block exit")
collectgarbage("collect")
assert(release_counts[close_id] == 1)

local err_close_id
local ok_err = pcall(function()
    local h <close> = java.new("java.lang.Object", "()V")
    err_close_id = h:id()
    error("boom")
end)
assert(not ok_err and live[err_close_id] == nil and release_counts[err_close_id] == 1)

-- 5) Manual java.release(ref) and java.release(raw_id) both disarm __gc.
local man_id, raw_id
do
    local a = java.new("A", "()V")
    man_id = a:id()
    assert(java.release(a) == true)
    assert(not a:is_alive() and a:id() == nil)
    -- Second ref:release() is an idempotent no-op.
    assert(a:release() == true)
    -- Using released ref as target fails cleanly.
    local used_ok = pcall(function() java.invoke(a, "returnInt", "()I") end)
    assert(not used_ok, "expected error on released java_ref")

    local b = java.new("B", "()V")
    raw_id = b:id()
    assert(java.release(raw_id) == true)
    assert(not b:is_alive())
end
collectgarbage("collect")
assert(release_counts[man_id] == 1 and release_counts[raw_id] == 1)

-- 6) loader .. ":Class", ref:invoke(), argument unwrapping, and ref:detach().
local detached_id
do
    local loader = java.load_dex("/tmp/test.dex")
    local lid = loader:id()
    assert(loader .. ":com.example.Foo" == tostring(lid) .. ":com.example.Foo")
    local arg_obj = java.new("Arg", "()V")
    local aid = arg_obj:id()
    assert(loader:invoke("returnInt", "(Ljava/lang/Object;)I", arg_obj) == 42)
    assert(last_invoke_args[1] == lid and last_invoke_args[4] == aid)
    detached_id = loader:detach()
    assert(detached_id == lid and not loader:is_alive())
end
collectgarbage("collect")
collectgarbage("collect")
assert(live[detached_id] == true, "detached handle must NOT be auto-released by GC")
assert(java.release(detached_id) == true and live[detached_id] == nil)

-- 7) java.scope(fn): LIFO release, nested scope:keep(), error propagation, nil-hole returns.
local s1, s2, kept_ref
local r1, r2, r3 = java.scope(function(outer)
    local a = java.new("Outer", "()V")
    s1 = a:id()
    kept_ref = java.scope(function(inner)
        local b = java.new("InnerTmp", "()V")
        local c = java.new("InnerKept", "()V")
        s2 = b:id()
        return inner:keep(c)
    end)
    assert(live[s2] == nil, "inner scope ref not released")
    assert(kept_ref:is_alive(), "kept ref must survive inner scope")
    outer:keep(kept_ref)
    return "ok", nil, 99
end)
assert(r1 == "ok" and r2 == nil and r3 == 99)
assert(live[s1] == nil, "outer scope ref not released")
assert(kept_ref:is_alive(), "kept ref must survive outer scope")
local kept_id = kept_ref:id()
kept_ref = nil
collectgarbage("collect")
collectgarbage("collect")
assert(live[kept_id] == nil, "escaped ref must still be GC-released when dropped")

-- 8) dejavu.scope(): LIFO cleanup + retryable busy semantics + <close>.
local events = {}
local rpc_busy = true
local fake_rpc = {
    unregister = function()
        events[#events + 1] = "rpc"
        if rpc_busy then
            return false, "busy"
        end
        return true
    end,
}
local jref = java.new("ScopedJava", "()V")
local jref_id = jref:id()

local sc = dejavu.scope()
sc:track_rpc(fake_rpc)
sc:track_hook(11)
sc:track_native_hook(22)
sc:track_stream("lease-1")
sc:track_java(jref)
sc:defer(function()
    events[#events + 1] = "defer"
    return true
end)
assert(sc:count() == 6)

-- First close: everything cleans up in LIFO order except busy RPC, which stays queued!
local closed, reason = sc:close()
assert(closed == false and reason == "busy")
assert(sc:count() == 1, "only the busy RPC entry should remain")
assert(live[jref_id] == nil)
assert(removed_hooks[1] == "stream:lease-1" and removed_hooks[2] == 11)
assert(removed_native_hooks[1] == 22)
assert(events[1] == "defer" and events[2] == "rpc")

-- Second close after RPC drains: succeeds without re-running already cleaned entries.
rpc_busy = false
closed, reason = sc:close()
assert(closed == true and reason == nil and sc:count() == 0)
assert(#events == 3 and events[3] == "rpc")

-- dejavu.scope() works with Lua 5.4 <close>.
local defer_ran = false
do
    local s <close> = dejavu.scope()
    s:defer(function() defer_ran = true end)
end
assert(defer_ran == true)

print("OK: Lua java_ref GC/<close>/scope and dejavu.scope lifecycle passed")
