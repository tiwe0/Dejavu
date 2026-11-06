package cafe.ivory.dejavu.data

object LuaPackageScripts {
    internal fun quote(value: String): String = buildString {
        append('"')
        value.forEach { character ->
            when {
                character == '"' -> append("\\\"")
                character == '\\' -> append("\\\\")
                character.code < 32 || character.code == 127 -> {
                    append('\\').append(character.code.toString().padStart(3, '0'))
                }
                else -> append(character)
            }
        }
        append('"')
    }

    private fun validateSession(session: String) {
        require(session.matches(Regex("[0-9a-f]{16}"))) { "Plugin session must contain 16 lowercase hex digits" }
    }

    private fun bounded(script: String): String {
        require(pluginUtf8Bytes(script).size <= MAX_PLUGIN_RPC_BYTES) { "Plugin RPC script exceeds 1 MiB" }
        return script
    }

    fun start(pkg: ImportedPluginPackage, session: String): String {
        pkg.validate()
        validateSession(session)
        val id = quote(pkg.manifest.id)
        val sessionValue = quote(session)
        val sources = pkg.manifest.files.joinToString(",\n") { path ->
            "[${quote(path)}]=${quote(pkg.sources.getValue(path))}"
        }
        return bounded("""
            local id, session = $id, $sessionValue
            local registry = rawget(_G, "__dejavu_app_packages")
            if registry == nil then registry = {}; rawset(_G, "__dejavu_app_packages", registry) end
            assert(type(registry) == "table", "plugin registry is invalid")
            local existing = registry[id]
            if existing ~= nil then
                assert(type(existing) == "table" and existing.session == session,
                       "plugin is owned by another session; stop it first")
                assert(existing.state == "running", "plugin cleanup is required; retry Stop")
                return id .. " already running"
            end
            local sources = {
            $sources
            }
            local entry, compile_error = load(sources[${quote(pkg.manifest.entry)}],
                ${quote("@dejavu-package/${pkg.manifest.id}/${pkg.manifest.entry}")}, "t", _G)
            assert(entry, compile_error)
            local ctx = {session = session}
            function ctx.read(path)
                assert(type(path) == "string" and sources[path] ~= nil, "undeclared plugin resource")
                return sources[path]
            end
            local record = {session = session, state = "error", ctx = ctx}
            registry[id] = record
            local ok, failure = pcall(function()
                record.api = entry()
                assert(type(record.api) == "table", "plugin entry must return a table")
                assert(type(record.api.start) == "function" and type(record.api.stop) == "function"
                       and type(record.api.status) == "function", "plugin must provide start/stop/status")
                assert(record.api.start(ctx) ~= false, "plugin start returned false")
            end)
            if not ok then
                local cleaned, cleanup_error = false, "plugin did not provide cleanup"
                if type(record.api) == "table" and type(record.api.stop) == "function" then
                    local stopped
                    cleaned, stopped = pcall(record.api.stop, ctx)
                    if cleaned and stopped == false then cleaned, stopped = false, "stop returned false" end
                    cleanup_error = stopped
                end
                if cleaned then registry[id] = nil else record.error = tostring(cleanup_error) end
                error(id .. " start failed: " .. tostring(failure) ..
                      (cleaned and "" or "; cleanup failed: " .. tostring(cleanup_error)), 0)
            end
            record.state = "running"
            return id .. " running"
        """.trimIndent())
    }

    fun stop(id: String, expectedSession: String? = null): String {
        validatePluginId(id)
        expectedSession?.let(::validateSession)
        val expected = expectedSession?.let(::quote) ?: "nil"
        return bounded("""
            local id, expected = ${quote(id)}, $expected
            local registry = rawget(_G, "__dejavu_app_packages")
            if registry == nil then return id .. " stopped" end
            assert(type(registry) == "table", "plugin registry is invalid")
            local record = registry[id]
            if record == nil then return id .. " stopped" end
            assert(type(record) == "table", "plugin record is invalid")
            if expected ~= nil and record.session ~= expected then
                return id .. " stop skipped: session changed"
            end
            assert(type(record.api) == "table" and type(record.api.stop) == "function",
                   "plugin cleanup is unavailable; restart the target process")
            local ok, result = pcall(record.api.stop, record.ctx)
            if not ok or result == false then
                record.state, record.error = "error", tostring(result)
                error(id .. " stop failed: " .. tostring(result), 0)
            end
            registry[id] = nil
            return id .. " stopped"
        """.trimIndent())
    }

    fun snapshot(): String = """
        local registry = rawget(_G, "__dejavu_app_packages")
        local lines = {"dejavu-packages-v1"}
        if registry == nil then return lines[1] end
        assert(type(registry) == "table", "plugin registry is invalid")
        local ids = {}
        for id in pairs(registry) do
            assert(type(id) == "string" and #id <= 64 and id:match("^[a-z][a-z0-9_-]*${'$'}"), "invalid plugin identifier")
            ids[#ids + 1] = id
        end
        table.sort(ids)
        for _, id in ipairs(ids) do
            local record = registry[id]
            assert(type(record) == "table" and type(record.session) == "string" and #record.session == 16
                   and record.session:match("^[0-9a-f]+${'$'}"), "invalid plugin record")
            if type(record.api) == "table" and type(record.api.status) == "function" then
                local ok = pcall(record.api.status, record.ctx)
                if not ok then record.state = "error" end
            end
            local state = record.state == "running" and "running" or "error"
            lines[#lines + 1] = id .. "\t" .. state .. "\t" .. record.session
        end
        return table.concat(lines, "\n")
    """.trimIndent()
}
