package cafe.ivory.dejavu.data

import android.content.Context
import java.io.File
import java.io.ByteArrayOutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import org.json.JSONObject

private fun rejected(block: () -> Unit) {
    check(runCatching(block).isFailure) { "Expected validation failure" }
}

fun main() {
    val provider = MemoryDocumentProvider()
    val library = NativePluginLibrary(Context(provider))
    val root = File(RootShell.mapped, "adb/dejavu/plugins")
    root.mkdirs()
    val elf = byteArrayOf(0x7f, 0x45, 0x4c, 0x46, 2, 1, 0, 0)
    val source = "int dejavu_jit_plugin_init(void) { return 0; }".toByteArray()
    fun manifest(id: String, kind: String, entry: String, files: List<String> = listOf(entry)) =
        PluginPackageManifest(id, "$id plugin", "1.2.3", "test description", entry, files, kind = kind).toJson().toByteArray()
    val uri = provider.tree(mapOf("manifest.json" to manifest("hello", "native", "bin/libdejavu_hello.so", listOf("bin/libdejavu_hello.so", "SKILL.md")),
        "bin/libdejavu_hello.so" to elf, "SKILL.md" to "safe instructions".toByteArray(), "ignored.c" to source))
    val jitUri = provider.tree(mapOf("manifest.json" to manifest("hellojit", "jit", "hello.c", listOf("hello.c", "SKILL.md")),
        "hello.c" to source, "SKILL.md" to "safe instructions".toByteArray()))
    check(!library.hasLuaManifest(uri))
    fun zipPackage(kind: String, entry: String, content: ByteArray): PluginZipArchive {
        val bytes = ByteArrayOutputStream().also { output ->
            ZipOutputStream(output).use { zip ->
                mapOf("manifest.json" to manifest("zip-$kind", kind, entry), entry to content).forEach { (name, data) ->
                    zip.putNextEntry(ZipEntry(name)); zip.write(data); zip.closeEntry()
                }
            }
        }.toByteArray()
        return PluginZipArchive.read(bytes.inputStream())
    }
    for ((kind, entry, bytes) in listOf(Triple("native", "plugin.so", elf), Triple("jit", "plugin.c", source))) {
        val archive = zipPackage(kind, entry, bytes)
        val zipArtifact = library.importPackage(archive.manifest, archive.files).single()
        val destination = File(root, "zip-$kind")
        check(zipArtifact.id == "zip-$kind" && File(destination, entry).readBytes().contentEquals(bytes))
        val before = File(destination, entry).readBytes()
        rejected { library.importPackage(archive.manifest, emptyMap()) }
        rejected { library.importPackage(archive.manifest, archive.files + ("undeclared.c" to source)) }
        rejected { library.importPackage(archive.manifest, mapOf(entry to "not a plugin".toByteArray())) }
        rejected { library.importPackage(archive.manifest, archive.files) { false } }
        if (kind == "native") {
            val tooDeep = (List(33) { "a" } + "plugin.so").joinToString("/")
            val deepArchive = zipPackage(kind, tooDeep, bytes)
            rejected { library.importPackage(deepArchive.manifest, deepArchive.files) }
        }
        check(File(destination, entry).readBytes().contentEquals(before))
        destination.deleteRecursively()
    }
    println("OK: ZIP native/JIT install validation and replacement guards preserve originals on rejection")
    val imported = library.importFolder(uri) + library.importFolder(jitUri)
    check(imported.size == 2)
    val commandsBeforeScan = RootShell.commandCount
    val listed = library.list()
    check(listed.map { it.path }.toSet() == imported.map { it.path }.toSet()) { library.lastScanErrors.toString() }
    check(library.lastScanErrors.isEmpty())
    check(RootShell.commandCount - commandsBeforeScan == 2) { "Native scanning must batch all manifest and resource checks into two root commands" }
    val batchFiles = listOf("bin/test.so") + (1..31).map { "resources/data-$it.txt" }
    val batchDirectory = File(root, "native-batch").apply { mkdirs() }
    File(batchDirectory, "manifest.json").writeBytes(manifest("native-batch", "native", "bin/test.so", batchFiles))
    batchFiles.forEach { path ->
        File(batchDirectory, path).apply {
            parentFile.mkdirs()
            writeBytes(if (path.endsWith(".so")) elf else byteArrayOf(1))
        }
    }
    val commandsBeforeBatch = RootShell.commandCount
    val sharedScan = PluginLibraryScan.read()
    check(library.list(sharedScan).size == 3) { library.lastScanErrors.toString() }
    check(RootShell.commandCount - commandsBeforeBatch == 2) { "32 native resources must use the same two scan commands" }
    File(batchDirectory, "bin/test.so").writeText("not an ELF")
    check(library.list().map { it.id }.toSet() == setOf("hello", "hellojit"))
    check(library.lastScanErrors.any { it.startsWith("native-batch:") && "bin/test.so" in it && "invalid ELF header" in it })
    batchDirectory.deleteRecursively()
    println("OK: batched native scans keep fixed root command counts and isolate invalid entry files")
    val largeChecks = (1..8).associate { "chunk-$it" to "# ${"x".repeat(10 * 1024)}\nprintf pass" }
    val commandsBeforeChunks = RootShell.commandCount
    val chunkResults = runPluginScanChecks(largeChecks)
    check(chunkResults.keys == largeChecks.keys && chunkResults.values.all { it.getOrThrow() == "pass" })
    check(RootShell.commandCount - commandsBeforeChunks == 2) { "Large scans must split before reaching su's argument limit" }
    check(imported.any { it.id == "hello" && it.name == "hello plugin" && it.version == "1.2.3" && !it.jit })
    check(imported.all { it.description == "test description" })
    check(!File(root, "hello/ignored.c").exists())
    val luaManifest = manifest("sample-lua", "lua", "app.lua", listOf("app.lua", "hook.c"))
    val lua = provider.tree(mapOf("manifest.json" to luaManifest, "app.lua" to "return {}".toByteArray(), "hook.c" to source))
    check(library.hasLuaManifest(lua))
    rejected { library.importFolder(lua) }
    rejected { library.hasLuaManifest(provider.tree(mapOf("hello.c" to source))) }
    rejected { library.importFolder(provider.tree(mapOf("hello.c" to source))) }
    rejected { library.importFolder(provider.tree(mapOf("dejavu-plugin.json" to luaManifest, "hello.c" to source))) }
    rejected { library.hasLuaManifest(provider.tree(mapOf("manifest.json" to "not json".toByteArray(), "hello.c" to source))) }
    rejected { library.importFolder(provider.tree(mapOf("manifest.json" to manifest("bad", "jit", "hello.c"), "hello.c" to "void hook(void) {}".toByteArray()))) }
    rejected { library.importFolder(provider.tree(mapOf("manifest.json" to manifest("bad", "native", "fake.so"), "fake.so" to "not elf".toByteArray()))) }
    rejected { library.importFolder(provider.tree(mapOf("manifest.json" to manifest("bad", "jit", "hello.c"), "hello.c" to ByteArray(512 * 1024 + 1)))) }
    rejected { library.importFolder(provider.tree(mapOf("manifest.json" to manifest("missing", "native", "missing.so")))) }
    val luaDirectory = File(root, "sample-lua").apply { mkdirs() }
    File(luaDirectory, "manifest.json").writeBytes(luaManifest)
    File(luaDirectory, "hook.c").writeBytes(source)
    File(root, "bare.so").writeBytes(elf)
    File(root, "ordinary.c").writeBytes(source)
    val unmanifested = File(root, "unmanifested").apply { mkdirs() }
    File(unmanifested, "hello.c").writeBytes(source)
    val legacy = File(root, "legacy").apply { mkdirs() }
    File(legacy, "dejavu-plugin.json").writeBytes(manifest("legacy", "jit", "hello.c"))
    File(legacy, "hello.c").writeBytes(source)
    val link = File(root, "linked")
    java.nio.file.Files.createSymbolicLink(link.toPath(), File(root, "hello").toPath())
    check(library.list().size == 2)
    check(library.lastScanErrors.isEmpty())
    val broken = File(root, "broken").apply { mkdirs() }
    File(broken, "manifest.json").writeText("not json")
    check(library.list().size == 2)
    check(library.lastScanErrors.size == 1)
    File(broken, "manifest.json").delete(); broken.delete(); link.delete()
    // Replacing a complete package retains only declared resources.
    library.importFolder(uri)
    check(library.list().size == 2)
    check(File(root, imported.first().path.substringAfter("$DEFAULT_PLUGIN_DIRECTORY/")).isFile)
    val replacement = provider.tree(mapOf("manifest.json" to manifest("hello", "native", "bin/libdejavu_hello.so", listOf("bin/libdejavu_hello.so", "SKILL.md")),
        "bin/libdejavu_hello.so" to elf + byteArrayOf(9), "SKILL.md" to "replacement instructions".toByteArray()))
    var replacementChecks = 0
    rejected {
        library.importFolder(replacement) { id ->
            check(id == "hello")
            replacementChecks++
            false
        }
    }
    check(replacementChecks == 1)
    check(File(root, "hello/bin/libdejavu_hello.so").readBytes().contentEquals(elf))
    check(File(root, "hello/SKILL.md").readText() == "safe instructions")
    replacementChecks = 0
    rejected {
        library.importFolder(replacement) { id ->
            check(id == "hello")
            ++replacementChecks == 1
        }
    }
    check(replacementChecks == 2)
    check(File(root, "hello/bin/libdejavu_hello.so").readBytes().contentEquals(elf))
    check(File(root, "hello/SKILL.md").readText() == "safe instructions")
    check(root.listFiles().orEmpty().none { it.name.startsWith(".native-") })
    val luaCollision = provider.tree(mapOf("manifest.json" to manifest("sample-lua", "native", "test.so"), "test.so" to elf))
    rejected { library.importFolder(luaCollision) }
    check(File(luaDirectory, "manifest.json").readBytes().contentEquals(luaManifest))
    check(File(luaDirectory, "hook.c").readBytes().contentEquals(source))
    check(!File(luaDirectory, "test.so").exists())
    val nativeCollision = provider.tree(mapOf("manifest.json" to manifest("hello", "jit", "hello.c"), "hello.c" to source))
    rejected { library.importFolder(nativeCollision) }
    check(File(root, "hello/bin/libdejavu_hello.so").readBytes().contentEquals(elf))
    check(!File(root, "hello/hello.c").exists())
    File(RootShell.properties.parentFile, "fail-next-import-commit").writeText("fail once")
    rejected { library.importFolder(replacement) }
    check(File(root, "hello/bin/libdejavu_hello.so").readBytes().contentEquals(elf))
    check(File(root, "hello/SKILL.md").readText() == "safe instructions")
    check(root.listFiles().orEmpty().none { it.name.startsWith(".native-") })
    val sourceEntry = File(root, "hello/bin/libdejavu_hello.so")
    sourceEntry.delete()
    java.nio.file.Files.createSymbolicLink(sourceEntry.toPath(), File(root, "bare.so").toPath())
    check(library.list().none { it.id == "hello" })
    check(library.lastScanErrors.isNotEmpty())
    sourceEntry.delete(); sourceEntry.writeBytes(elf)
    val artifact = imported.first { !it.jit }
    val process = "com.example.test"
    File(RootShell.mapped, "user_de/0/$process/files").mkdirs()
    val old = JSONObject().put("debug.dejavu.plugins.allowed.0", "/data/existing.so")
        .put("debug.dejavu.plugins.sha256.0", "a".repeat(64))
        .put("debug.dejavu.plugins.skill.sha256.0", "b".repeat(64))
    RootShell.properties.writeText(old.toString())
    val destination = library.deploy(process, artifact)
    check(library.matchesDeployment(process, artifact, destination))
    check(!library.matchesDeployment("com.other.app", artifact, destination))
    check(library.deployedPath(process, artifact) == destination)
    check(File(RootShell.mapped, destination.removePrefix("/data/")).readBytes().contentEquals(elf))
    val props = JSONObject(RootShell.properties.readText())
    check(props.getString("debug.dejavu.plugins.allowed.0") == "/data/existing.so")
    check(props.getString("debug.dejavu.plugins.sha256.0") == "a".repeat(64))
    check(props.getString("debug.dejavu.plugins.skill.sha256.0") == "b".repeat(64))
    check(props.getString("debug.dejavu.plugins.allowed.1").endsWith(artifact.deploymentName))
    check(props.getString("debug.dejavu.plugins.sha256.1").matches(Regex("[0-9a-f]{64}")))
    check(props.getString("debug.dejavu.plugins.skill.sha256.1").matches(Regex("[0-9a-f]{64}")))
    check(library.deploy(process, artifact) == destination)
    val otherClient = RootShell.start("""
        exec 9>/data/adb/dejavu/.plugin-trust.lock
        flock -n 9 || exit 1
        echo OTHER_CLIENT_READY
        read -r release || true
    """.trimIndent())
    try {
        check(otherClient.inputStream.bufferedReader().readLine() == "OTHER_CLIENT_READY")
        val beforeConflict = RootShell.properties.readText()
        val result = runCatching { library.deploy(process, artifact) }
        check(result.isFailure && result.exceptionOrNull()?.message?.contains("busy") == true)
        check(RootShell.properties.readText() == beforeConflict)
    } finally {
        otherClient.outputStream.close()
        check(otherClient.waitFor(2, java.util.concurrent.TimeUnit.SECONDS))
    }
    check(library.deploy(process, artifact) == destination)
    val deployed = File(RootShell.mapped, destination.removePrefix("/data/"))
    val deployedSkill = File(deployed.path + ".SKILL.md")
    val beforePlugin = deployed.readBytes()
    val beforeSkill = deployedSkill.readBytes()
    val sourceFile = File(RootShell.mapped, artifact.path.removePrefix("/data/"))
    sourceFile.writeBytes(elf + byteArrayOf(1, 2, 3))
    File(root, "hello/SKILL.md").writeText("new instructions")
    File(RootShell.properties.parentFile, "fail-next-hash-write").writeText("fail once")
    rejected { library.deploy(process, artifact) }
    check(deployed.readBytes().contentEquals(beforePlugin))
    check(deployedSkill.readBytes().contentEquals(beforeSkill))
    check(JSONObject(RootShell.properties.readText()).toString() == props.toString())
    File(RootShell.properties.parentFile, "change-slot-on-copy").writeText("race")
    rejected { library.deploy(process, artifact) }
    check(deployed.readBytes().contentEquals(beforePlugin))
    check(deployedSkill.readBytes().contentEquals(beforeSkill))
    check(JSONObject(RootShell.properties.readText()).getString("debug.dejavu.plugins.allowed.1") == "/data/concurrently-installed.so")
    RootShell.properties.writeText(props.toString())
    for (slot in 2 until 16) props.put("debug.dejavu.plugins.allowed.$slot", "/data/other$slot.so")
    RootShell.properties.writeText(props.toString())
    val jit = imported.first { it.jit }
    rejected { library.deploy(process, jit) }
    check(JSONObject(RootShell.properties.readText()).toString() == props.toString())
    rejected { library.deploy("../escape", artifact) }
    println("OK: manifest-only discovery/import guards, shared trust locking, deployment/slot rollback and trust preservation")
}
