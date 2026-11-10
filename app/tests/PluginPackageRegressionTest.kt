package cafe.ivory.dejavu.data

import java.io.File
import java.util.concurrent.TimeUnit

private inline fun packageRejects(block: () -> Unit) {
    check(runCatching(block).isFailure) { "invalid package was accepted" }
}

fun runPluginPackageRegressionTests() {
    val text = """{"schemaVersion":1,"kind":"lua","id":"example","name":"中文插件","version":"1","entry":"entry.lua","files":["entry.lua","lib/hook.c"],"events":{"tag":"DejavuExample","marker":"DJEX","format":"urlencoded-json-v1"}}"""
    val manifest = PluginPackageManifest.parse(text)
    check(manifest.name == "中文插件" && manifest.events?.marker == "DJEX")
    packageRejects { PluginPackageManifest.parse(text.replace("\"schemaVersion\":1", "\"schemaVersion\":2")) }
    packageRejects { PluginPackageManifest.parse(text.replace("\"kind\":\"lua\"", "\"kind\":\"native\"")) }
    for (path in listOf("../entry.lua", "/entry.lua", "C:/entry.lua", "lib/../entry.lua", "lib//entry.lua", "lib\\entry.lua", "./entry.lua")) {
        packageRejects { manifest.copy(files = listOf(path), entry = path).validate() }
    }
    packageRejects { manifest.copy(id = "../example").validate() }
    packageRejects { manifest.copy(files = listOf("entry.lua", "entry.lua")).validate() }
    packageRejects { manifest.copy(entry = "missing.lua").validate() }
    packageRejects { manifest.copy(files = listOf("entry.lua", "entry.lua/hook.c")).validate() }
    packageRejects { manifest.copy(files = (1..33).map { "$it.lua" }, entry = "1.lua").validate() }
    val sources = linkedMapOf("entry.lua" to "return {}", "lib/hook.c" to "中文\u0000\n\r\"\\123")
    val pkg = ImportedPluginPackage(manifest, sources)
    pkg.validate()
    packageRejects { pkg.copy(sources = sources + ("extra.lua" to "return {}" )).validate() }
    packageRejects { pkg.copy(sources = sources - "entry.lua").validate() }
    packageRejects { pkg.copy(sources = sources + ("entry.lua" to "x".repeat(MAX_PLUGIN_PACKAGE_BYTES + 1))).validate() }
    check(decodePluginUtf8("中文".toByteArray()) == "中文")
    packageRejects { decodePluginUtf8(byteArrayOf(0xc3.toByte(), 0x28)) }
    packageRejects { pkg.copy(sources = sources + ("entry.lua" to "\uD800")).validate() }
    val script = LuaPackageScripts.start(pkg, "0123456789abcdef")
    check(script.toByteArray().size <= MAX_PLUGIN_RPC_BYTES)
    check("\u0000" !in script && "\\000" in script && "\\010" in script)
    packageRejects { LuaPackageScripts.start(pkg, "bad\nsession") }
    packageRejects { LuaPackageScripts.start(pkg, "ABCDEF0123456789") }
    packageRejects { LuaPackageScripts.start(pkg, "0123456789abcde") }
    packageRejects { LuaPackageScripts.stop("../example") }
    val expandedManifest = manifest.copy(files = listOf("entry.lua"), events = null)
    val expanded = ImportedPluginPackage(expandedManifest, mapOf(
        "entry.lua" to "\u0000".repeat(MAX_PLUGIN_PACKAGE_BYTES - pluginUtf8Bytes(expandedManifest.toJson()).size),
    ))
    expanded.validate()
    packageRejects { LuaPackageScripts.start(expanded, "0123456789abcdef") }
    println("OK: plugin manifest, declared resources, UTF-8, limits and Lua source quoting")
    val repository = File(System.getenv("DEJAVU_REPO_ROOT") ?: ".")
    runPluginPageRegression(manifest, repository)
    val example = readTestPackage(File(repository, "app/tests/fixtures/example-plugin"))
    check(example.manifest.id == "example-plugin" && example.manifest.entry == "entry.lua")
    check(example.manifest.files.containsAll(listOf("entry.lua", "helper.lua", "hook.c")))
    LuaPackageScripts.start(example, "0123456789abcdef")
    runPackageLifecycleRegression(manifest, example)
    println("OK: self-contained example folder satisfies the app package and RPC limits")
}

private fun runPluginPageRegression(legacy: PluginPackageManifest, repository: File) {
    check(PluginPackageManifest.parse(legacy.toJson()) == legacy)
    val legacyEvents = requireNotNull(legacy.events)
    check(!legacyEvents.usesSocket && legacy.ui == null)
    val manifest = PluginPackageManifest.parse("""
        {
          "schemaVersion": 1, "kind": "lua", "id": "page", "name": "Page",
          "version": "1", "entry": "entry.lua", "files": ["entry.lua", "ui.json"],
          "ui": "ui.json",
          "events": {"format": "hook-socket-json-v1", "topic": "example.message"}
        }
    """.trimIndent())
    check(PluginPackageManifest.parse(manifest.toJson()) == manifest)
    val socketEvents = requireNotNull(manifest.events)
    check(socketEvents.usesSocket && socketEvents.topic == "example.message")
    packageRejects { manifest.copy(ui = "absent.json").validate() }
    packageRejects { manifest.copy(entry = "ui.json").validate() }
    packageRejects { manifest.copy(ui = "entry.lua").validate() }
    packageRejects {
        manifest.copy(kind = "native", entry = "test.so", files = listOf("test.so", "ui.json"), events = null).validate()
    }
    packageRejects { manifest.copy(events = socketEvents.copy(topic = "bad topic")).validate() }
    packageRejects { manifest.copy(events = socketEvents.copy(tag = "Conflicting")).validate() }
    packageRejects { legacy.copy(events = legacyEvents.copy(marker = "")).validate() }
    packageRejects { legacy.copy(events = legacyEvents.copy(topic = "invalid")).validate() }

    val source = """
        {
          "version": 1,
          "title": {"en": "Status", "zh": "状态"},
          "blocks": [
            {"type": "text", "text": "Help"},
            {"type": "status", "bind": "runtime.state"},
            {"type": "button", "label": "Check", "action": "check"},
            {"type": "button", "label": "Trigger", "action": "call", "call": "emit_1"},
            {"type": "input", "label": "Port", "bind": "events.port"},
            {"type": "events", "limit": 50}
          ]
        }
    """.trimIndent()
    fun packageWithPage(text: String, metadata: PluginPackageManifest = manifest) =
        ImportedPluginPackage(metadata, mapOf("entry.lua" to "return {}", "ui.json" to text))
    val pkg = packageWithPage(source).also { it.validate() }
    val page = requireNotNull(pkg.page)
    check(page.title == PluginPageText("Status", "状态"))
    check(page.blocks == listOf(
        PluginPageBlock.Text(PluginPageText("Help")),
        PluginPageBlock.Status,
        PluginPageBlock.Button(PluginPageText("Check"), "check"),
        PluginPageBlock.Button(PluginPageText("Trigger"), "call", "emit_1"),
        PluginPageBlock.Input(PluginPageText("Port")),
        PluginPageBlock.Events(50),
    ))
    check(page.preset == null && page.eventLimit == 50)
    check(page.hasAction("emit_1") && !page.hasAction("check"))
    for (invalid in listOf(
        source.replace("\"version\": 1", "\"version\": 2"),
        source.replace("\"version\": 1", "\"version\": 1.0"),
        source.replace("\"runtime.state\"", "\"arbitrary.state\""),
        source.replace("\"action\": \"check\"", "\"action\": \"exec\""),
        source.replace("\"call\": \"emit_1\"", "\"call\": \"return os.execute()\""),
        source.replace("\"action\": \"call\"", "\"action\": \"start\""),
        source.replace("\"call\": \"emit_1\"", "\"extra\": \"emit_1\""),
        source.replace("\"en\": \"Status\", \"zh\": \"状态\"", "\"zh\": \"状态\""),
        source.replace("\"Port\"", "\"\""),
        source.replace("\"limit\": 50", "\"limit\": 51"),
        source.replace("\"limit\": 50", "\"limit\": 0"),
        source.replace("""{"type": "events", "limit": 50}""", """{"type":"status","bind":"runtime.state"}"""),
        source.replace("""{"type": "events", "limit": 50}""", """{"type":"events","limit":1},{"type":"events","limit":1}"""),
        source.replace("""{"type": "input", "label": "Port", "bind": "events.port"}""",
            """{"type":"input","label":"1","bind":"events.port"},{"type":"input","label":"2","bind":"events.port"}"""),
        source.replace("\"Help\"", "\"\\q\""),
        source.replace("\"Help\"", "\"\\ud800\""),
        source.replace("\"Help\"", "'" + "\"".repeat(2) + "bad'"),
        source.replace("\"Help\"", "[".repeat(1024) + "0" + "]".repeat(1024)),
        source + " false",
        " ".repeat(32 * 1024) + source,
    )) {
        packageRejects { packageWithPage(invalid).validate() }
    }
    packageRejects { packageWithPage(source, manifest.copy(events = legacy.events)).validate() }
    packageRejects { packageWithPage(source, manifest.copy(events = null)).validate() }
    packageRejects {
        packageWithPage("""{"version":1,"title":"Status","blocks":[{"type":"events","limit":5}]}""",
            manifest.copy(events = null)).validate()
    }
    val logPage = packageWithPage(
        """{"version":1,"title":"Status","blocks":[{"type":"events","limit":5}]}""",
        manifest.copy(events = legacy.events),
    ).also { it.validate() }
    check(logPage.page?.blocks == listOf(PluginPageBlock.Events(5)))
    println("OK: optional plugin page, bounded schema, legacy events and socket metadata")
    runPluginPagePresetRegression(manifest, legacyEvents)
    runPluginPageCompiler(repository, manifest)
}

private fun runPluginPagePresetRegression(manifest: PluginPackageManifest, legacyEvents: PluginEventSpec) {
    val socketEvents = requireNotNull(manifest.events)
    val monitor = """
        {
          "version": 2, "title": {"en": "Watch", "zh": "监控"}, "preset": "monitor",
          "actions": [{"id": "sample", "label": {"en": "Emit", "zh": "发送"}}]
        }
    """.trimIndent()
    val monitorPackage = ImportedPluginPackage(manifest, mapOf(
        "entry.lua" to "return {}", "ui.json" to monitor,
    )).also { it.validate() }
    val watch = requireNotNull(monitorPackage.page)
    val sample = PluginPageAction("sample", PluginPageText("Emit", "发送"))
    check(watch.title == PluginPageText("Watch", "监控") && watch.blocks.isEmpty())
    check(watch.preset == PluginPagePreset.Monitor(50, listOf(sample)))
    check(watch.eventLimit == 50 && watch.hasAction("sample") && !watch.hasAction("stop"))
    val logcat = PluginPage.parse("""{"version":2,"title":"Watch","preset":"monitor","eventLimit":1}""", legacyEvents)
    check(logcat.preset == PluginPagePreset.Monitor(1, emptyList()) && logcat.eventLimit == 1)
    packageRejects { PluginPage.parse(monitor, null) }

    val controls = """
        {
          "version": 2, "title": "Control", "preset": "controls",
          "rows": [{"label": {"en": "Mode", "zh": "模式"}, "value": {"en": "Test", "zh": "测试"}}],
          "actions": [{"id": "sample", "label": {"en": "Emit", "zh": "发送"}}]
        }
    """.trimIndent()
    val controller = PluginPage.parse(controls, null)
    check(controller.preset == PluginPagePreset.Controls(
        listOf(PluginPageInfoRow(PluginPageText("Mode", "模式"), PluginPageText("Test", "测试"))),
        listOf(sample),
    ))
    check(controller.eventLimit == null && controller.hasAction("sample") && !controller.hasAction("start"))
    check(PluginPage.parse("""{"version":2,"title":"Control","preset":"controls"}""", null).preset ==
        PluginPagePreset.Controls(emptyList(), emptyList()))

    val tooManyActions = (1..9).joinToString(",") { """{"id":"action_$it","label":"Emit"}""" }
    val tooManyRows = (1..17).joinToString(",") { """{"label":"Mode $it","value":"Test"}""" }
    for (invalid in listOf(
        monitor.replace("\"preset\": \"monitor\"", "\"preset\": \"stack\""),
        monitor.replace("\"preset\": \"monitor\"", "\"preset\": \"monitor\", \"eventLimit\": 0"),
        monitor.replace("\"preset\": \"monitor\"", "\"preset\": \"monitor\", \"eventLimit\": 51"),
        monitor.replace("\"preset\": \"monitor\"", "\"preset\": \"monitor\", \"eventLimit\": 1.0"),
        monitor.replace("\"actions\"", "\"rows\""),
        monitor.replace("\"id\": \"sample\"", "\"id\": \"bad/action\""),
        monitor.replace("\"id\": \"sample\"", "\"id\": \"sample\", \"action\": \"stop\""),
        controls.replace("\"preset\": \"controls\"", "\"preset\": \"controls\", \"eventLimit\": 4"),
        controls.replace("\"label\": {\"en\": \"Mode\", \"zh\": \"模式\"}", "\"label\": \"\""),
        """{"version":2,"title":"Control","preset":"controls","actions":{}}""",
        """{"version":2,"title":"Control","preset":"controls","rows":null}""",
        """{"version":2,"title":"Watch","preset":"monitor","blocks":[]}""",
        """{"version":2,"title":"Watch","preset":"monitor","actions":[{"id":"sample","label":"Emit"},{"id":"sample","label":"Again"}]}""",
        """{"version":2,"title":"Control","preset":"controls","actions":[$tooManyActions]}""",
        """{"version":2,"title":"Control","preset":"controls","rows":[$tooManyRows]}""",
    )) {
        packageRejects { PluginPage.parse(invalid, socketEvents) }
    }
    println("OK: monitor and controls presets, action allowlists and strict bounds")
}

private fun runPluginPageCompiler(repository: File, manifest: PluginPackageManifest) {
    val lua = File(System.getenv("DEJAVU_TEST_LUA") ?: File(repository, "zygisk/out/host-tests/lua").path)
    if (!lua.canExecute()) {
        println("SKIP: Lua plugin page compiler requires a built host Lua")
        return
    }
    val file = File.createTempFile("dejavu-plugin-page-", ".lua")
    try {
        fun compile(source: String): Pair<Int, String> {
            file.writeText(source)
            val process = ProcessBuilder(
                lua.absolutePath, File(repository, "scripts/compile-plugin-ui.lua").absolutePath, file.absolutePath,
            ).redirectErrorStream(true).start()
            check(process.waitFor(5, TimeUnit.SECONDS)) {
                process.destroyForcibly()
                "Lua plugin page compiler timed out"
            }
            return process.exitValue() to process.inputStream.bufferedReader().readText()
        }
        val source = """
            return {
                version = 1,
                title = { zh = "测试😀", en = "Test" },
                blocks = {
                    { type = "text", text = "Quote: \" newline:\n backslash:\\" },
                    { type = "input", bind = "events.port", label = "Port" },
                    { type = "events", limit = 3 },
                },
            }
        """.trimIndent()
        val (status, json) = compile(source)
        check(status == 0) { json }
        check(compile(source) == (status to json))
        val page = PluginPage.parse(json, manifest.events)
        check(page.title == PluginPageText("Test", "测试😀"))
        check(page.blocks.first() == PluginPageBlock.Text(PluginPageText("Quote: \" newline:\n backslash:\\")))
        check(page.blocks.last() == PluginPageBlock.Events(3))
        val (presetStatus, presetJson) = compile("""
            return {
                version = 2, title = "Watch", preset = "monitor",
                actions = { { id = "sample", label = { en = "Emit", zh = "发送" } } },
            }
        """.trimIndent())
        check(presetStatus == 0) { presetJson }
        check(PluginPage.parse(presetJson, manifest.events).preset ==
            PluginPagePreset.Monitor(50, listOf(PluginPageAction("sample", PluginPageText("Emit", "发送")))))
        val (emptyStatus, emptyJson) = compile("""
            return { version = 2, title = "Control", preset = "controls", actions = {}, rows = {} }
        """.trimIndent())
        check(emptyStatus == 0) { emptyJson }
        check(PluginPage.parse(emptyJson, null).preset == PluginPagePreset.Controls(emptyList(), emptyList()))
        for (bad in listOf(
            """return { title = io.open("unexpected") }""",
            """return { title = ("x"):rep(8) }""",
            """local s = "x"; while true do s = s .. s end""",
            """local t = {}; t.cycle = t; return t""",
            """return { [2] = "hole" }""",
            """return { title = "\255" }""",
            """return { x = {{{{}}}} }""",
            "while true do end",
        )) {
            check(compile(bad).first != 0) { "Unsafe page source was accepted: $bad" }
        }
        println("OK: deterministic Lua page compiler, UTF-8 and execution bounds")
    } finally {
        file.delete()
    }
}

private fun readTestPackage(folder: File): ImportedPluginPackage {
    val manifest = PluginPackageManifest.parse(decodePluginUtf8(File(folder, PLUGIN_MANIFEST_FILE).readBytes()))
    return ImportedPluginPackage(manifest, manifest.files.associateWith { path ->
        decodePluginUtf8(File(folder, path).readBytes())
    }).also { it.validate() }
}

/** Emits code-only RPC scripts for a manual device lifecycle check. */
fun emitPluginPackageScripts(repository: File, output: File, session: String = "0123456789abcdef") {
    val pkg = readTestPackage(File(repository, "app/tests/fixtures/example-plugin"))
    val start = LuaPackageScripts.start(pkg, session)
    check(output.isDirectory || output.mkdirs()) { "Unable to create script output directory" }
    File(output, "start.lua").writeText(start)
    File(output, "stop.lua").writeText(LuaPackageScripts.stop(pkg.manifest.id, session))
    File(output, "snapshot.lua").writeText(LuaPackageScripts.snapshot())
    File(output, "session.txt").writeText(session + "\n")
    println("Emitted app package RPC scripts to ${output.absolutePath}")
}

private fun longLuaString(value: String): String {
    var equals = ""
    while ("]$equals]" in value) equals += "="
    return "[$equals[\n$value]$equals]"
}

private fun runPackageLifecycleRegression(manifest: PluginPackageManifest, example: ImportedPluginPackage) {
    val lua = System.getenv("DEJAVU_TEST_LUA") ?: "zygisk/out/host-tests/lua"
    if (!File(lua).canExecute()) {
        println("SKIP: Lua package lifecycle requires DEJAVU_TEST_LUA or make -C zygisk test")
        return
    }
    val session = "0123456789abcdef"
    val otherSession = "fedcba9876543210"
    val good = ImportedPluginPackage(manifest, mapOf(
        "entry.lua" to """
            return {
              start = function(ctx)
                assert(ctx.session == "$session")
                assert(ctx.read("lib/hook.c") == "中文" .. string.char(0,10,13,34,92) .. "123]===]")
                assert(not pcall(ctx.read, "../secret"))
                starts = starts + 1
              end,
              stop = function(ctx) assert(ctx.session == "$session"); stops = stops + 1 end,
              status = function(ctx) return "running" end
            }
        """.trimIndent(),
        "lib/hook.c" to "中文\u0000\n\r\"\\123]===]",
    ))
    val broken = ImportedPluginPackage(manifest.copy(id = "broken"), mapOf(
        "entry.lua" to """
            return {
              start = function(ctx) error("partial start failure") end,
              stop = function(ctx)
                cleanup_calls = cleanup_calls + 1
                if not cleanup_allowed then error("retry cleanup") end
              end,
              status = function(ctx) return "error" end
            }
        """.trimIndent(),
        "lib/hook.c" to "",
    ))
    fun run(script: String) = "run(${longLuaString(script)})"
    val script = """
        local function run(source) return assert(load(source, "@package-test", "t", _G))() end
        _G.__dejavu_app_packages = nil
        starts, stops, cleanup_calls = 0, 0, 0
        assert(${run(LuaPackageScripts.snapshot())} == "dejavu-packages-v1")
        ${run(LuaPackageScripts.start(good, session))}
        ${run(LuaPackageScripts.start(good, session))}
        assert(starts == 1, "same-session start duplicated effects")
        assert(not pcall(run, ${longLuaString(LuaPackageScripts.start(good, otherSession))}))
        assert(${run(LuaPackageScripts.snapshot())} == "dejavu-packages-v1\nexample\trunning\t$session")
        ${run(LuaPackageScripts.stop("example", otherSession))}
        assert(stops == 0, "another session stopped this package")
        ${run(LuaPackageScripts.stop("example", session))}
        assert(stops == 1 and ${run(LuaPackageScripts.snapshot())} == "dejavu-packages-v1")
        cleanup_allowed = true
        assert(not pcall(run, ${longLuaString(LuaPackageScripts.start(broken, session))}))
        assert(cleanup_calls == 1 and ${run(LuaPackageScripts.snapshot())} == "dejavu-packages-v1")
        cleanup_allowed = false
        assert(not pcall(run, ${longLuaString(LuaPackageScripts.start(broken, session))}))
        assert(${run(LuaPackageScripts.snapshot())} == "dejavu-packages-v1\nbroken\terror\t$session")
        assert(not pcall(run, ${longLuaString(LuaPackageScripts.start(broken, otherSession))}))
        assert(not pcall(run, ${longLuaString(LuaPackageScripts.stop("broken", session))}))
        assert(cleanup_calls == 3, "failed cleanup ownership was lost")
        cleanup_allowed = true
        ${run(LuaPackageScripts.stop("broken", session))}
        assert(cleanup_calls == 4 and ${run(LuaPackageScripts.snapshot())} == "dejavu-packages-v1")
        ${run(LuaPackageScripts.start(example, session))}
        assert(${run(LuaPackageScripts.snapshot())} == "dejavu-packages-v1\n${example.manifest.id}\trunning\t$session")
        ${run(LuaPackageScripts.stop(example.manifest.id, session))}
        assert(${run(LuaPackageScripts.snapshot())} == "dejavu-packages-v1")
        print("OK: Lua package ownership, idempotence, quoting and retryable start/stop cleanup")
    """.trimIndent()
    val file = File.createTempFile("dejavu-package-lifecycle-", ".lua")
    try {
        file.writeText(script)
        val process = ProcessBuilder(lua, file.absolutePath).redirectErrorStream(true).start()
        check(process.waitFor(10, TimeUnit.SECONDS)) {
            process.destroyForcibly()
            "Lua package lifecycle test timed out"
        }
        val output = process.inputStream.bufferedReader().readText()
        check(process.exitValue() == 0) { output }
        print(output)
    } finally {
        file.delete()
    }
}
