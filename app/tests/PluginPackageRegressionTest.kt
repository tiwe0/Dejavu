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
    val example = readTestPackage(File(repository, "app/tests/fixtures/example-plugin"))
    check(example.manifest.id == "example-plugin" && example.manifest.entry == "entry.lua")
    check(example.manifest.files.containsAll(listOf("entry.lua", "helper.lua", "hook.c")))
    LuaPackageScripts.start(example, "0123456789abcdef")
    runPackageLifecycleRegression(manifest, example)
    println("OK: self-contained example folder satisfies the app package and RPC limits")
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
