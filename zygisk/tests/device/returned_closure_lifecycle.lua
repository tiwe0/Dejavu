-- Supply the allowed device C fixture path through returned_closure_plugin_path.
local path = assert(returned_closure_plugin_path, "missing returned closure fixture path")
local function must_reject(fn)
    local ok, message = pcall(fn)
    assert(not ok and tostring(message):find("no longer loaded", 1, true), tostring(message))
end
plugin.load_jit(path)
local factory = closurejit.make
local nested = factory()
local leaf = nested()
assert(type(nested) == "function" and type(leaf) == "function")
assert(leaf() > 0)
plugin.unload("closurejit")
must_reject(factory)
must_reject(nested)
must_reject(leaf)
plugin.load_jit(path)
assert(closurejit.make()()() > 0)
must_reject(factory)
must_reject(nested)
must_reject(leaf)
plugin.unload("closurejit")
return "OK: returned host functions are recursively guarded and revoked on unload/reload"
