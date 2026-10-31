-- Minimal DEX class-name reader used by the Lua unpack example.

local dex = {}

local function u32(data, offset)
    local a, b, c, d = data:byte(offset + 1, offset + 4)
    if not a then return nil end
    return a + b * 256 + c * 65536 + d * 16777216
end

local function read_uleb(data, offset)
    local value, shift = 0, 0
    while true do
        local byte = data:byte(offset + 1)
        if not byte then return nil, offset end
        offset = offset + 1
        value = value + (byte % 128) * (2 ^ shift)
        if byte < 128 then return value, offset end
        shift = shift + 7
        if shift > 35 then return nil, offset end
    end
end

function dex.is_dex(data)
    return type(data) == 'string' and type(data.sub) == 'function' and
        (data:sub(1, 4) == 'dex\n' or data:sub(1, 5) == 'cdex0')
end

function dex.class_names(data, namespace, extra_filter)
    local names, seen = {}, {}
    if not dex.is_dex(data) then return names end
    local string_ids_size = u32(data, 0x38)
    local string_ids_offset = u32(data, 0x3c)
    local type_ids_size = u32(data, 0x40)
    local type_ids_offset = u32(data, 0x44)
    local class_defs_size = u32(data, 0x60)
    local class_defs_offset = u32(data, 0x64)
    if not string_ids_size or not class_defs_size or class_defs_size == 0 then
        return names
    end
    for index = 0, class_defs_size - 1 do
        local class_index = u32(data, class_defs_offset + index * 32)
        if class_index < type_ids_size then
            local string_index = u32(data, type_ids_offset + class_index * 4)
            local string_offset = u32(data, string_ids_offset + string_index * 4)
            local _, data_offset = read_uleb(data, string_offset)
            if data_offset then
                local start = data_offset
                while data_offset < #data and data:byte(data_offset + 1) ~= 0 do
                    data_offset = data_offset + 1
                end
                local descriptor = data:sub(start + 1, data_offset)
                if descriptor:sub(1, 1) == 'L' then
                    local name = descriptor:sub(2, -2):gsub('/', '.')
                    local matches = namespace == nil or namespace == '' or
                        name == namespace or name:sub(1, #namespace + 1) == namespace .. '.'
                    if matches and (not extra_filter or extra_filter == '' or
                        name:find(extra_filter, 1, true) ~= nil) and
                        not seen[name] then
                        seen[name] = true
                        names[#names + 1] = name
                        if #names >= 600 then return names end
                    end
                end
            end
        end
    end
    return names
end

return dex
