-- Active invocation driven by the Manifest main activity namespace.
-- The namespace is the package of the launcher activity class; classes are
-- collected from dumped DEX files in the output directory and invoked in
-- chunks with the FART-style null-thread sentinel.

local function u16(data, offset)
    local a, b = data:byte(offset + 1, offset + 2)
    return a + b * 256
end

local function u32(data, offset)
    local a, b, c, d = data:byte(offset + 1, offset + 4)
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

local function collect_dex_classes(data, namespace, extra_filter, names, seen)
    local string_ids_size = u32(data, 0x38)
    local string_ids_offset = u32(data, 0x3c)
    local type_ids_size = u32(data, 0x40)
    local type_ids_offset = u32(data, 0x44)
    local class_defs_size = u32(data, 0x60)
    local class_defs_offset = u32(data, 0x64)
    if string_ids_size == 0 or type_ids_size == 0 or class_defs_size == 0 then
        return
    end
    for index = 0, class_defs_size - 1 do
        if #names >= 600 then return end
        local class_index = u32(data, class_defs_offset + index * 32)
        if class_index < type_ids_size then
            local string_index = u32(data, type_ids_offset + class_index * 4)
            local name = class_descriptor(data, string_ids_offset, string_index)
            if name and not seen[name] and
                (name == namespace or name:sub(1, #namespace + 1) == namespace .. '.') and
                (not extra_filter or extra_filter == '' or
                 name:find(extra_filter, 1, true) ~= nil) then
                seen[name] = true
                names[#names + 1] = name
            end
        end
    end
end

local function main_activity_namespace()
    local app = java.invoke('android.app.ActivityThread', 'currentApplication',
        '()Landroid/app/Application;')
    local package = java.invoke(app, 'getPackageName', '()Ljava/lang/String;')
    local manager = java.invoke(app, 'getPackageManager',
        '()Landroid/content/pm/PackageManager;')
    local intent = java.invoke(manager, 'getLaunchIntentForPackage',
        '(Ljava/lang/String;)Landroid/content/Intent;', package)
    local class_name = nil
    if intent ~= nil then
        local component = java.invoke(intent, 'getComponent',
            '()Landroid/content/ComponentName;')
        if component ~= nil then
            class_name = java.invoke(component, 'getClassName', '()Ljava/lang/String;')
            java.release(component)
        end
        java.release(intent)
    end
    java.release(manager)
    java.release(app)
    if class_name and class_name:sub(1, 1) == '.' then
        class_name = package .. class_name
    end
    local namespace = class_name and class_name:match('^(.*)%.') or package
    return namespace, package, class_name
end

function active_invoke_manifest(dex_directory, extra_filter)
    local namespace, package, class_name = main_activity_namespace()
    local names = {}
    local seen = {}
    local listing = fs.list(dex_directory)
    local candidates = {}
    for file in listing:gmatch('[^\n]+') do
        local path = dex_directory .. '/' .. file
        local stat = fs.stat(path)
        local size = stat and tonumber(stat:match('size=(%d+)'))
        if size and (file:sub(-4) == '.dex' or file:sub(-5) == '.cdex') and
            size < 64 * 1024 * 1024 then
            candidates[#candidates + 1] = {file = file, size = size}
        end
    end
    table.sort(candidates, function(left, right) return left.size > right.size end)
    local dex_count = 0
    local parsed_bytes = 0
    for _, candidate in ipairs(candidates) do
        if dex_count >= 4 or parsed_bytes >= 64 * 1024 * 1024 then break end
        local path = dex_directory .. '/' .. candidate.file
        local data = fs.read(path)
        if data:sub(1, 4) == 'dex\n' or data:sub(1, 5) == 'cdex0' then
            dex_count = dex_count + 1
            parsed_bytes = parsed_bytes + #data
            collect_dex_classes(data, namespace, extra_filter, names, seen)
        end
        if #names >= 600 then break end
    end
    if #names == 0 then
        return string.format(
            'namespace=%s package=%s main_activity=%s dex_files=%d classes=0 ' ..
            '(no matching classes in dumped dex)', namespace, package,
            tostring(class_name), dex_count)
    end
    if not unpack.hook_invoke() then error('unpack.hook_invoke failed') end
    unpack.active_invoke_async(table.concat(names, ','), 40)
    local completed = unpack.active_invoke_wait(300000)
    unpack.flush()
    return string.format(
        'namespace=%s package=%s main_activity=%s dex_files=%d classes=%d\n%s',
        namespace, package, tostring(class_name), dex_count, #names, completed)
end
