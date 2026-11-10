-- Usage: lua scripts/compile-plugin-ui.lua path/to/ui.lua > path/to/ui.json
local MAX_BYTES = 32 * 1024
local MAX_INSTRUCTIONS = 20000
local MAX_HEAP_GROWTH_KIB = 1024
local MAX_TABLE_DEPTH = 4

local function load_page(path)
    local file = assert(io.open(path, "rb"))
    local source = file:read(MAX_BYTES + 1) or ""
    file:close()
    assert(#source <= MAX_BYTES, "plugin page source exceeds 32 KiB")
    local chunk = assert(load(source, "@" .. path, "t", {}))

    -- Lua strings normally expose the string library through their metatable.
    -- Remove that access while running the page source.
    local string_metatable = debug.getmetatable("")
    debug.setmetatable("", nil)
    local instructions, heap_start = 0, collectgarbage("count")
    debug.sethook(function()
        instructions = instructions + 1
        if instructions > MAX_INSTRUCTIONS or collectgarbage("count") - heap_start > MAX_HEAP_GROWTH_KIB then
            debug.sethook()
            error("plugin page exceeded its execution budget", 0)
        end
    end, "", 1)
    local ok, page = pcall(chunk)
    debug.sethook()
    debug.setmetatable("", string_metatable)
    assert(ok, page)
    assert(type(page) == "table", "plugin page must return a table")
    return page
end

local function encode_page(page)
    local parts, bytes, active = {}, 0, {}
    local function append(part)
        bytes = bytes + #part
        assert(bytes < MAX_BYTES, "plugin page JSON exceeds 32 KiB")
        parts[#parts + 1] = part
    end
    local function encode_string(value)
        assert(#value < MAX_BYTES and utf8.len(value), "plugin page strings must be bounded UTF-8")
        append('"' .. value:gsub('[%z\1-\31\\"]', function(char)
            if char == '"' or char == '\\' then return '\\' .. char end
            return string.format("\\u%04x", char:byte())
        end) .. '"')
    end
    local function encode(value, depth)
        local kind = type(value)
        if kind == "string" then
            encode_string(value)
        elseif kind == "number" and math.type(value) == "integer" then
            append(tostring(value))
        elseif kind == "boolean" then
            append(tostring(value))
        elseif kind == "table" then
            assert(depth <= MAX_TABLE_DEPTH, "plugin page is nested too deeply")
            assert(not active[value], "plugin page contains a cyclic table")
            active[value] = true
            local keys, numeric, maximum = {}, 0, 0
            for key in pairs(value) do
                if math.type(key) == "integer" and key > 0 then
                    numeric = numeric + 1
                    maximum = math.max(maximum, key)
                else
                    assert(type(key) == "string", "plugin page table keys must be strings or positive integers")
                end
                keys[#keys + 1] = key
            end
            if numeric > 0 then
                assert(numeric == #keys and maximum == numeric, "plugin page array must be dense")
                append("[")
                for index = 1, numeric do
                    if index > 1 then append(",") end
                    encode(value[index], depth + 1)
                end
                append("]")
            else
                table.sort(keys)
                append("{")
                for index, key in ipairs(keys) do
                    if index > 1 then append(",") end
                    encode_string(key)
                    append(":")
                    local child = value[key]
                    if depth == 1 and page.version == 2 and (key == "actions" or key == "rows")
                        and type(child) == "table" and next(child) == nil then
                        append("[]")
                    else
                        encode(child, depth + 1)
                    end
                end
                append("}")
            end
            active[value] = nil
        else
            error("unsupported plugin page value: " .. kind, 0)
        end
    end
    encode(page, 1)
    return table.concat(parts)
end

assert(#arg == 1, "usage: lua scripts/compile-plugin-ui.lua path/to/ui.lua")
io.write(encode_page(load_page(arg[1])), "\n")
