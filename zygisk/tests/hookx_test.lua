local root = assert(arg[1], "missing zygisk root path")
local installs = {}
local original_tointeger = math.tointeger

hook = {
    install = function(class_name, method_name, signature, source)
        installs[#installs + 1] = {
            class_name = class_name,
            method_name = method_name,
            signature = signature,
            source = source,
        }
        return 1000 + #installs
    end,
    list = function()
        return ""
    end,
    remove = function()
        return true
    end,
    uninstall = function()
        return true
    end,
    clear = function()
        return 0
    end,
    log = function()
    end,
}

local loaded_ok, loaded_error
math.tointeger = nil
loaded_ok, loaded_error = pcall(function()
    assert(loadfile(root .. "/config/hookx.lua"))()
end)
math.tointeger = original_tointeger
assert(loaded_ok, loaded_error)
assert(type(hookx) == "table")

local function last_source()
    return installs[#installs].source
end

local function expect_error(fragment, callback)
    local ok, err = pcall(callback)
    assert(not ok, "expected error containing: " .. fragment)
    assert(type(err) == "string" and err:find(fragment, 1, true), err)
end

assert(hookx.sig({ret = "void"}) == "()V")
assert(
    hookx.sig({
        args = {"int", "boolean", "String", "[I", "Lcom/foo/Bar;"},
        ret = "[Ljava/lang/String;",
    }) == "(IZLjava/lang/String;[ILcom/foo/Bar;)[Ljava/lang/String;")

expect_error("hookx.sig ret is required", function()
    hookx.sig({args = {"int"}})
end)
expect_error("void is valid only as a return type", function()
    hookx.sig({args = {"void"}, ret = "void"})
end)
expect_error("unsupported type name 'java.lang.String'", function()
    hookx.sig({args = {"java.lang.String"}, ret = "void"})
end)
expect_error("invalid object descriptor", function()
    hookx.sig({args = {"Lbad"}, ret = "void"})
end)
expect_error("truncated JNI descriptor", function()
    hookx.sig({args = {"["}, ret = "void"})
end)
expect_error("JNI signature has trailing data", function()
    hookx.trace("android.app.Activity", "onResume", "()VV")
end)
expect_error("invalid object descriptor", function()
    hookx.trace("android.app.Activity", "onResume", "(Lbad.;)V")
end)
expect_error("new_value must be an integer", function()
    hookx.replace_arg_int("com.example.Target", "compute", "(I)I", 0, 1.5)
end)
expect_error("value must fit in C int range", function()
    hookx.force_return_int("com.example.Target", "check", "()I", 2147483648)
end)

local trace_id = hookx.trace(
    "android.app.Activity",
    "onResume",
    "()V",
    {message = "line\n\"quoted\"\\slash"})
assert(trace_id == 1001)
assert(last_source():find("int before_hook", 1, true))
assert(not last_source():find("after_hook", 1, true))
assert(last_source():find("hookx.trace android.app.Activity#onResume()V", 1, true))
assert(last_source():find("line\\n\\\"quoted\\\"\\\\slash", 1, true))

local replace_id = hookx.replace_arg_int(
    "com.example.Target",
    "compute",
    "(II)I",
    1,
    7)
assert(replace_id == 1002)
assert(last_source():find("dejavu_hook_set_arg_int(context, 1, 7)", 1, true))
expect_error("hookx.replace_arg_int requires argument #0 to be I", function()
    hookx.replace_arg_int("com.example.Target", "bad", "(Z)I", 0, 1)
end)
expect_error("argument index must be non-negative", function()
    hookx.replace_arg_int("com.example.Target", "bad", "(I)I", -1, 1)
end)

local force_int_id = hookx.force_return_int(
    "com.example.Target",
    "check",
    "()I",
    42)
assert(force_int_id == 1003)
assert(last_source():find("return dejavu_hook_return_int(context, 42);", 1, true))
expect_error("hookx.force_return_int requires return type I", function()
    hookx.force_return_int("com.example.Target", "bad", "()Z", 1)
end)

local force_bool_id = hookx.force_return_bool(
    "com.example.Target",
    "check",
    "()Z",
    true)
assert(force_bool_id == 1004)
assert(last_source():find("return dejavu_hook_return_boolean(context, 1);", 1, true))
expect_error("hookx.force_return_bool requires return type Z", function()
    hookx.force_return_bool("com.example.Target", "bad", "()I", true)
end)

local force_void_id = hookx.force_return_void(
    "android.app.Activity",
    "onResume",
    "()V")
assert(force_void_id == 1005)
assert(last_source():find("return dejavu_hook_return_void(context);", 1, true))
expect_error("hookx.force_return_void requires return type V", function()
    hookx.force_return_void("com.example.Target", "bad", "()I")
end)

local saved_hook = hook
hook = nil
expect_error("hook.install is unavailable in this Lua session", function()
    hookx.trace("android.app.Activity", "onResume", "()V")
end)
hook = saved_hook

local string_log_id = hookx.log_string_arg(
    "com.example.Target",
    "submit",
    "(Ljava/lang/String;)V",
    0)
assert(string_log_id == 1006)
assert(last_source():find("dejavu_hook_get_arg_string_mutf8(", 1, true))
assert(last_source():find("context, 0, buffer, sizeof(buffer), &size);", 1, true))
assert(last_source():find("DEJAVU_HOOK_ERROR_RANGE", 1, true))
assert(last_source():find("hookx.log_string_arg value:", 1, true))
expect_error("hookx.log_string_arg requires argument #0 to be Ljava/lang/String;", function()
    hookx.log_string_arg("com.example.Target", "bad", "(I)V", 0)
end)

local result_log_id = hookx.log_result_int(
    "com.example.Target",
    "compute",
    "(I)I")
assert(result_log_id == 1007)
assert(last_source():find("int after_hook", 1, true))
assert(last_source():find("dejavu_hook_get_result_int(context, &value)", 1, true))
assert(last_source():find("hookx_log_signed", 1, true))
expect_error("hookx.log_result_int requires return type I", function()
    hookx.log_result_int("com.example.Target", "bad", "(I)Z")
end)

print("OK: hookx Lua helpers")
