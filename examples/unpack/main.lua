-- Lua-side unpack strategy: Manifest namespace -> dumped DEX classes ->
-- paced per-class active invocation -> revision dump.
--
-- Native layer only provides hook/dump primitives:
--   unpack.enable/hook_invoke/active_invoke/dump_revision/flush/sleep_ms
--
-- Usage (from CLI):
--   python3 examples/unpack/cli.py -p <process> --out <device-dir> --active-invoke-manifest
-- Advanced: concatenate dex.lua + manifest.lua + main.lua and call
--   run_unpack_example({dex_dir=..., class_filter=..., delay_ms=40})

local function list_dex_files(directory)
    local candidates = {}
    for file in fs.list(directory):gmatch('[^\n]+') do
        if file:sub(-4) == '.dex' or file:sub(-5) == '.cdex' then
            local stat = fs.stat(directory .. '/' .. file)
            local size = stat and tonumber(stat:match('size=(%d+)'))
            if size and size < 64 * 1024 * 1024 then
                candidates[#candidates + 1] = {file = file, size = size}
            end
        end
    end
    table.sort(candidates, function(left, right) return left.size > right.size end)
    return candidates
end

function run_unpack_example(config)
    config = config or {}
    local status = unpack.status()
    local dex_directory = config.dex_dir
    if not dex_directory then
        dex_directory = status:match('dir=([^%s]+)')
    end
    if not dex_directory or dex_directory == '' or dex_directory == 'nil' then
        error('unpack example: no output directory; enable unpacking first')
    end
    local namespace, package, main_activity = manifest.main_activity()
    local names, seen = {}, {}
    local dex_files = 0
    local function add_names(parsed)
        for _, name in ipairs(parsed) do
            if not seen[name] then
                seen[name] = true
                names[#names + 1] = name
                if #names >= 600 then return end
            end
        end
    end
    if config.dex_file then
        local data = fs.read(config.dex_file)
        add_names(dex.class_names(data, namespace, config.class_filter))
        dex_files = 1
    else
        for index, candidate in ipairs(list_dex_files(dex_directory)) do
            if index > 4 then break end
            local data = fs.read(dex_directory .. '/' .. candidate.file)
            add_names(dex.class_names(data, namespace, config.class_filter))
            dex_files = dex_files + 1
            if #names >= 600 then break end
        end
    end
    if #names == 0 and config.fallback_classes then
        for name in config.fallback_classes:gmatch('[^,;%s]+') do
            names[#names + 1] = name
        end
    end
    if #names == 0 then
        return string.format(
            'namespace=%s package=%s main_activity=%s dex_files=%d classes=0 ' ..
            '(no matching classes in dumped dex)', namespace, package,
            tostring(main_activity), dex_files)
    end
    unpack.hook_invoke()
    local delay_ms = tonumber(config.delay_ms) or 40
    local calls = 0
    for index, name in ipairs(names) do
        unpack.active_invoke(name)
        calls = calls + 1
        if index % 10 == 0 then
            unpack.sleep_ms(delay_ms)
        end
        if index % 100 == 0 then
            unpack.flush(5000)
        end
    end
    pcall(unpack.dump_revision)
    unpack.flush(120000)
    return string.format(
        'namespace=%s package=%s main_activity=%s dex_files=%d classes=%d calls=%d\n%s',
        namespace, package, tostring(main_activity), dex_files, #names, calls,
        unpack.status())
end
