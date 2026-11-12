-- Push fixture.dex, libdejavu_native_namespace.so and replacement.c to an app
-- files directory. Set the three globals below to their absolute device paths.
-- This fixture deliberately retains its ClassLoader handle until process exit:
-- uninstall does not wait for entered replacements or saved-original users.
local dex = assert(native_namespace_dex, "missing native_namespace_dex")
local library = assert(native_namespace_library, "missing native_namespace_library")
local source_file = assert(native_namespace_source_file, "missing native_namespace_source_file")
if not native_namespace_fixture_loader then
    native_namespace_fixture_loader = java.load_dex(dex)
    native_namespace_fixture_class = native_namespace_fixture_loader .. ":io.dejavu.bridge.NativeNamespaceFixture"
    java.invoke(native_namespace_fixture_class, "load", "(Ljava/lang/String;)V", library)
    native_namespace_fixture_library = library
end
assert(native_namespace_fixture_library == library,
    "namespace fixture already loaded from another path; restart the target before changing libraries")
local class = native_namespace_fixture_class
local function call(value)
    return java.invoke(class, "call", "(J)J", value)
end
local function permissions()
    return java.invoke(class, "permissions", "()I")
end
local function protect(value)
    return java.invoke(class, "protect", "(I)I", value)
end
local function require_caller_owned(id)
    local basename = assert(library:match("[^/]+$"))
    for line in native.list():gmatch("[^\n]+") do
        if line:match("^" .. id .. " ") then
            assert(line:find(basename .. "#", 1, true),
                "cannot verify namespace fixture: dladdr did not identify the dedicated library")
            assert(line:find(" lifetime=caller-owned", 1, true),
                "cannot verify namespace isolation: Agent retained a module reference to the fixture")
            return
        end
    end
    error("installed namespace fixture hook missing from native.list")
end
local replacement = [[
long long dejavu_native_target(long long value) {
    long long (*original)(long long) = (void *)dejavu_native_original();
    if (!original) return -90001;
    dejavu_native_hit();
    return original(value) + 100;
}
]]
local address = java.invoke(class, "address", "()J")
assert(type(address) == "number" and address > 0 and address % 4 == 0, "invalid fixture address")
local initial = permissions()
assert(initial == 4 or initial == 5, "fixture target is not executable")
local id
local ok, result = pcall(function()
    assert(call(9) == 16, "fixture baseline changed")
    assert(protect(4) == 0 and permissions() == 4, "unable to set fixture XOM")
    for _, from_file in ipairs({ false, true }) do
        if from_file then
            id = native.install_address_file(string.format("0x%x", address), source_file)
        else
            id = native.install_address(address, replacement)
        end
        require_caller_owned(id)
        assert(permissions() == 4, "address installation changed XOM permissions")
        assert(call(9) == 116, "replacement must modify the callable original's result")
        assert(native.stats(id) >= 1, "replacement hit was not counted")
        assert(native.uninstall(id))
        id = nil
        assert(permissions() == 4, "uninstall changed XOM permissions")
        assert(call(9) == 16, "uninstall did not restore the original result")
    end
    return "OK: identified caller-owned ClassLoader image; integer/hex-file hooks, original, hits, uninstall and XOM"
end)
local cleaned, cleanup_error = true, nil
if id then cleaned, cleanup_error = pcall(native.uninstall, id) end
local restored, restore_result = pcall(protect, initial)
if not ok then error(result .. (cleaned and "" or "; hook cleanup failed: " .. tostring(cleanup_error))) end
assert(cleaned, cleanup_error)
assert(restored and restore_result == 0 and permissions() == initial, "unable to restore fixture permissions")
return result
