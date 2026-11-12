-- Dedicated library only: no system libc patching or application data writes.
local library = assert(native_fixture_library, "missing native fixture library path")
local rounds = native_fixture_rounds or 10
assert(type(rounds) == "number" and rounds >= 1 and rounds <= 100)
local function call(symbol, value)
    return native.call(library, symbol, [[
void dejavu_native_call(void) {
    long long (*target)(long long) = (void *)(unsigned long long)DEJAVU_NATIVE_CALL_ADDRESS;
    dejavu_native_call_return(target(dejavu_native_call_argument_int(0)));
}
]], value or 0)
end
local replacement = [[
long long dejavu_native_target(long long value) {
    long long (*original)(long long) = (void *)dejavu_native_original();
    if (!original) return -90001;
    dejavu_native_hit();
    return original(value);
}
]]
local id
local initial = call("native_fixture_permissions")
local address = module.resolve(library, "native_fixture_target")
local hex_address = string.format("0x%x", address)
local function install(round)
    if round % 3 == 1 then return native.install(library, "native_fixture_target", replacement) end
    if round % 3 == 2 then return native.install_address(address, replacement) end
    return native.install_address(hex_address, replacement)
end
local ok, result = pcall(function()
    -- This target has no exported symbol: address dispatch must reach it directly.
    local hidden = call("native_fixture_hidden_address")
    assert(call("native_fixture_hidden_call", 5) == 22)
    id = native.install_address(hidden, [[
long long dejavu_native_target(long long value) {
    long long (*original)(long long) = (void *)dejavu_native_original();
    dejavu_native_hit();
    return original(value) + 100;
}
]])
    assert(call("native_fixture_hidden_call", 5) == 122, "hidden address hook did not modify the result")
    assert(native.uninstall(id))
    id = nil
    assert(call("native_fixture_hidden_call", 5) == 22, "hidden target was not restored")
    local source_path = native_fixture_source_path
    if source_path then
        id = native.install_address_file(hex_address, source_path)
        assert(call("native_fixture_target", 9) == 16)
        assert(native.uninstall(id))
        id = nil
    end
    for _, bad in ipairs({0, -1, 1.0, true, "0x", "0x10000000000000000", "0x0"}) do
        assert(not pcall(native.install_address, bad, replacement), "invalid address accepted")
    end
    assert(not pcall(native.install_address, address + 1, replacement), "unaligned address accepted")
    assert(not pcall(native.install_address, 4, replacement), "unmapped address accepted")
    assert(call("native_fixture_start") == 0)
    assert(call("native_fixture_protect", 4) == 0)
    assert(call("native_fixture_permissions") == 4)
    assert(not pcall(native.install, library, "native_fixture_target", "invalid C !"))
    assert(call("native_fixture_permissions") == 4, "compile failure changed XOM permissions")
    assert(not pcall(native.install, library, "native_fixture_target", "int other_entry(void) { return 0; }"))
    assert(call("native_fixture_permissions") == 4, "missing entry changed XOM permissions")
    assert(not pcall(native.install_address, address, "invalid C !"))
    assert(call("native_fixture_permissions") == 4, "address compile failure changed XOM permissions")
    for i = 1, rounds do
        id = install(i)
        assert(call("native_fixture_permissions") == 4, "install left target readable/writable")
        assert(call("native_fixture_target", 9) == 16)
        assert(not pcall(native.install, library, "native_fixture_target", replacement), "duplicate hook accepted")
        assert(not pcall(native.install_address, address, replacement), "cross-entry duplicate hook accepted")
        assert(native.stats(id) > 0, "replacement was never called")
        assert(native.uninstall(id))
        id = nil
        assert(call("native_fixture_permissions") == 4, "uninstall changed original XOM permissions")
        assert(call("native_fixture_target", 9) == 16)
        assert(call("native_fixture_errors") == 0, "concurrent call observed an invalid original")
    end
    local count = call("native_fixture_calls")
    assert(count > 0)
    return "OK: hidden address hook, device source=" .. (source_path and "verified" or "not supplied") ..
        ", strict validation; " .. rounds ..
        " mixed symbol/address XOM install/uninstall cycles; concurrent_calls=" .. count
end)
if id then pcall(native.uninstall, id) end
local stopped, errors = pcall(call, "native_fixture_stop")
local restored, restore_result = pcall(call, "native_fixture_protect", initial)
if not ok then error(result) end
assert(stopped and errors == 0, "fixture workers reported invalid calls")
assert(restored and restore_result == 0, "unable to restore fixture permissions")
return result
