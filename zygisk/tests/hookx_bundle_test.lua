local root = assert(arg[1], "missing zygisk root path")
local logs = {}
local installs = {}

local function read_file(path)
    local handle = assert(io.open(path, "rb"))
    local content = assert(handle:read("*a"))
    assert(handle:close())
    return content
end

hook = {
    install = function(class_name, method_name, signature, source)
        installs[#installs + 1] = {
            class_name = class_name,
            method_name = method_name,
            signature = signature,
            source = source,
        }
        return 2000 + #installs
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
    log = function(message)
        logs[#logs + 1] = message
    end,
}

local bundled_init = read_file(root .. "/config/hookx.lua") ..
    "\n" ..
    read_file(root .. "/config/init.lua")
assert(load(bundled_init, "@bundled-init.lua"))()

assert(type(hookx) == "table")
assert(logs[#logs] == "Dejavu Lua control ready (hookx available)")

local id = hookx.trace("android.app.Activity", "onResume", "()V")
assert(id == 2001)
assert(installs[1].source:find("hookx.trace android.app.Activity#onResume()V", 1, true))
assert(installs[1].source:find("hookx.log_result_int integer log overflow", 1, true) == nil)

print("OK: bundled init exposes hookx")
