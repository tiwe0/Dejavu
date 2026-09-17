local root = assert(arg[1], "missing zygisk root path")
local cc = os.getenv("CC") or "cc"
local installs = {}

local function write_file(path, content)
    local handle = assert(io.open(path, "wb"))
    assert(handle:write(content))
    assert(handle:close())
end

local function remove_if_exists(path)
    if path ~= nil then
        os.remove(path)
    end
end

local function assert_command(command)
    local ok, reason, code = os.execute(command)
    if ok == true or ok == 0 then
        return
    end
    error(("command failed (%s %s): %s"):format(tostring(reason), tostring(code), command))
end

local function shell_quote(value)
    return "'" .. value:gsub("'", [['"'"']]) .. "'"
end

hook = {
    install = function(class_name, method_name, signature, source)
        installs[#installs + 1] = {
            class_name = class_name,
            method_name = method_name,
            signature = signature,
            source = source,
        }
        return 3000 + #installs
    end,
}

assert(loadfile(root .. "/config/hookx.lua"))()
assert(type(hookx) == "table")

local stub_preamble = [[
typedef struct dejavu_hook_context dejavu_hook_context;
enum dejavu_hook_status {
    DEJAVU_HOOK_OK = 0,
    DEJAVU_HOOK_ERROR_RANGE = -4
};
extern void dejavu_hook_log(const char *message);
extern int dejavu_hook_get_arg_string_mutf8(
    dejavu_hook_context *, unsigned int, char *, unsigned long, unsigned long *);
extern int dejavu_hook_get_result_int(dejavu_hook_context *, int *);
#define DEJAVU_HOOK_TRY(expression)                 \
    do {                                            \
        int dejavu_hook_status__ = (expression);    \
        if (dejavu_hook_status__ != DEJAVU_HOOK_OK) \
            return dejavu_hook_status__;            \
    } while (0)
]]

local function compile_generated_source(name, source)
    assert(not source:find("%f[%a_]size_t%f[^%a_]", 1), name .. " unexpectedly uses size_t")
    local path = os.tmpname() .. ".c"
    local ok, err = pcall(function()
        write_file(path, stub_preamble .. "\n" .. source)
        assert_command(
            table.concat({
                shell_quote(cc),
                "-std=gnu17",
                "-Werror",
                "-fsyntax-only",
                shell_quote(path),
            }, " "))
    end)
    remove_if_exists(path)
    assert(ok, err)
end

hookx.log_string_arg("com.example.Target", "submit", "(Ljava/lang/String;)V", 0)
compile_generated_source("hookx.log_string_arg", installs[#installs].source)

hookx.log_result_int("com.example.Target", "compute", "(I)I")
compile_generated_source("hookx.log_result_int", installs[#installs].source)

print("OK: hookx generated C compiles without stddef helpers")
