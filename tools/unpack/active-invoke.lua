-- Extract class names from a dumped DEX file and trigger FART-style active
-- invocation in chunks. Requires: unpack.enable(dir) already called and the
-- ArtMethod::Invoke sentinel available (unpack.hook_invoke()).
--
-- Used by: ./dejavu unpack --active-invoke-dex <device-dex-path> [--class-filter <prefix>]

local function u16(data, offset)
    local a, b = data:byte(offset + 1, offset + 2)
    return a + b * 256
end

local function u32(data, offset)
    local a, b, c, d = data:byte(offset + 1, offset + 4)
    return a + b * 256 + c * 65536 + d * 16777216
end

local function read_uleb(data, offset)
    local value = 0
    local shift = 0
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

local function class_descriptor(data, string_ids_offset, string_index)
    local string_data_offset = u32(data, string_ids_offset + string_index * 4)
    local _, data_offset = read_uleb(data, string_data_offset)
    if not data_offset then return nil end
    local start = data_offset
    while data_offset < #data and data:byte(data_offset + 1) ~= 0 do
        data_offset = data_offset + 1
    end
    local descriptor = data:sub(start + 1, data_offset)
    if descriptor:sub(1, 1) ~= 'L' then return nil end
    return descriptor:sub(2, -2):gsub('/', '.')
end

function active_invoke_dex(dex_path, filter)
    if type(dex_path) ~= 'string' then error('dex path required') end
    if not fs.exists(dex_path) then error('dex file not found: ' .. dex_path) end
    local data = fs.read(dex_path)
    if data:sub(1, 4) ~= 'dex\n' then error('not a dex file: ' .. dex_path) end
    local string_ids_size = u32(data, 0x38)
    local string_ids_offset = u32(data, 0x3c)
    local type_ids_size = u32(data, 0x40)
    local type_ids_offset = u32(data, 0x44)
    local class_defs_size = u32(data, 0x60)
    local class_defs_offset = u32(data, 0x64)
    if string_ids_size == 0 or type_ids_size == 0 or class_defs_size == 0 then
        error('dex has no class definitions')
    end
    local names = {}
    for index = 0, class_defs_size - 1 do
        local class_index = u32(data, class_defs_offset + index * 32)
        if class_index < type_ids_size then
            local string_index = u32(data, type_ids_offset + class_index * 4)
            local name = class_descriptor(data, string_ids_offset, string_index)
            if name and (not filter or filter == '' or name:find(filter, 1, true)) then
                names[#names + 1] = name
                if #names >= 2000 then break end
            end
        end
    end
    if #names == 0 then return 'no matching classes in ' .. dex_path end
    if not unpack.hook_invoke() then error('unpack.hook_invoke failed') end
    unpack.active_invoke_async(table.concat(names, ','), 40)
    local completed = unpack.active_invoke_wait(300000)
    unpack.flush()
    return string.format('classes=%d\n%s', #names, completed)
end

