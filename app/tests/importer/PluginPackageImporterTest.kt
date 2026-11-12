package cafe.ivory.dejavu.data

import android.content.Context
import android.net.Uri
import org.json.JSONObject
import java.io.File
import java.io.ByteArrayOutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.attribute.PosixFilePermissions
import java.security.MessageDigest

private inline fun expectImportFailure(label: String, block: () -> Unit): Throwable {
    return runCatching(block).exceptionOrNull() ?: error("$label unexpectedly succeeded")
}

private fun digest(bytes: ByteArray): String = MessageDigest.getInstance("SHA-256")
    .digest(bytes).joinToString("") { "%02x".format(it.toInt() and 255) }

private fun directorySnapshot(directory: File): Map<String, String> {
    check(directory.isDirectory)
    return Files.walk(directory.toPath()).use { paths ->
        paths.filter { Files.isRegularFile(it, LinkOption.NOFOLLOW_LINKS) }.toList().associate { path ->
            directory.toPath().relativize(path).toString() to digest(Files.readAllBytes(path))
        }
    }
}

private fun noTransactionDirectories(store: File) {
    check(store.listFiles().orEmpty().none { it.name.startsWith(".import-") || it.name.startsWith(".backup-") }) {
        "An import left staging or backup directories behind"
    }
}

fun main() {
    val sandbox = File(checkNotNull(System.getenv("DEJAVU_IMPORTER_SANDBOX"))).canonicalFile
    check(File(sandbox, ".host-only").readText() == "dejavu-importer-host-only\n")
    val repository = File(checkNotNull(System.getenv("DEJAVU_REPO_ROOT")))
    val folder = File(repository, "app/tests/fixtures/example-plugin")
    val original = PluginPackageManifest.parse(File(folder, PLUGIN_MANIFEST_FILE).readText())
    check(original.id == "example-plugin")
    val code = original.files.associateWith { File(folder, it).readBytes() }
    fun providerFiles(manifest: PluginPackageManifest, sources: Map<String, ByteArray> = code): Map<String, ByteArray> =
        sources + (PLUGIN_MANIFEST_FILE to manifest.toJson().toByteArray())

    val provider = MemoryDocumentProvider()
    val importer = PluginPackageImporter(Context(provider))
    val store = File(sandbox, "mapped-data/adb/dejavu/plugins")
    fun rootCommands(): Int = File(sandbox, "audit.jsonl").takeIf { it.exists() }?.readLines()?.count {
        JSONObject(it).getString("tool") == "su"
    } ?: 0
    check(importer.list().isEmpty() && importer.lastScanErrors.isEmpty())
    val uri = provider.tree(providerFiles(original))
    val imported = importer.importFolder(uri)
    check(imported.manifest == original)
    check(imported.sources == code.mapValues { decodePluginUtf8(it.value) })
    val commandsBeforeScan = rootCommands()
    check(importer.list() == listOf(imported) && importer.lastScanErrors.isEmpty())
    check(rootCommands() - commandsBeforeScan == 2) { "Library scanning must batch manifest and source reads into two root commands" }
    val installed = File(store, "example-plugin")
    check(Files.getPosixFilePermissions(store.toPath()) == PosixFilePermissions.fromString("rwx------"))
    check(Files.getPosixFilePermissions(File(installed, "hook.c").toPath()) == PosixFilePermissions.fromString("rw-------"))
    val originalDisk = directorySnapshot(installed)
    noTransactionDirectories(store)
    println("OK: real example plugin folder imports/scans via opaque SAF IDs and root stdin, without executing entry code")

    val batchFiles = listOf("entry.lua") + (1..31).map { "resources/source-$it.lua" }
    val batchManifest = PluginPackageManifest("many-resources", "Many resources", "1", entry = "entry.lua", files = batchFiles)
    val batchDirectory = File(store, batchManifest.id).apply { mkdirs() }
    File(batchDirectory, PLUGIN_MANIFEST_FILE).writeText(batchManifest.toJson())
    batchFiles.forEach { path ->
        File(batchDirectory, path).apply {
            parentFile.mkdirs()
            writeText(if (path == batchFiles.last()) "" else "return {}\n")
        }
    }
    val commandsBeforeBatch = rootCommands()
    val sharedScan = PluginLibraryScan.read()
    val batch = importer.list(sharedScan).single { it.manifest.id == batchManifest.id }
    check(batch.sources.keys == batchFiles.toSet())
    check(rootCommands() - commandsBeforeBatch == 2) { "32-resource packages must use the same two scan commands" }
    val diagnosticBody = "return 'scan-source-body-must-not-appear-in-errors'"
    File(batchDirectory, "entry.lua").writeText(diagnosticBody)
    val missingPath = batchFiles[1]
    File(batchDirectory, missingPath).delete()
    check(importer.list().map { it.manifest.id } == listOf(original.id)) { importer.lastScanErrors.toString() }
    val missingError = importer.lastScanErrors.single { it.startsWith("many-resources:") }
    check(missingPath in missingError && "missing" in missingError) { "Missing source diagnostics must identify the relative path and reason: $missingError" }
    check(diagnosticBody !in missingError && java.util.Base64.getEncoder().encodeToString(diagnosticBody.toByteArray()) !in missingError)
    check(missingError.length <= 1024)
    File(batchDirectory, missingPath).writeText("return {}")
    File(batchDirectory, "entry.lua").writeBytes(byteArrayOf(0xc3.toByte(), 0x28))
    check(importer.list().map { it.manifest.id } == listOf(original.id)) { importer.lastScanErrors.toString() }
    check(importer.lastScanErrors.any { it.startsWith("many-resources:") })
    batchDirectory.deleteRecursively()

    val oversizedManifest = PluginPackageManifest("oversized-stored", "Oversized", "1", entry = "entry.lua", files = listOf("entry.lua", "one.c", "two.c"))
    val oversizedDirectory = File(store, oversizedManifest.id).apply { mkdirs() }
    File(oversizedDirectory, PLUGIN_MANIFEST_FILE).writeText(oversizedManifest.toJson())
    File(oversizedDirectory, "entry.lua").writeText("return {}")
    File(oversizedDirectory, "one.c").writeText("x".repeat(180 * 1024))
    File(oversizedDirectory, "two.c").writeText("x".repeat(90 * 1024))
    check(importer.list().map { it.manifest.id } == listOf(original.id)) { importer.lastScanErrors.toString() }
    check(importer.lastScanErrors.any { it.startsWith("oversized-stored:") })
    oversizedDirectory.deleteRecursively()
    println("OK: batched scanning uses two root commands for 32 resources and preserves aggregate size rejection")

    fun zipPackage(manifest: PluginPackageManifest, resources: Map<String, ByteArray>): PluginZipArchive {
        val bytes = ByteArrayOutputStream().also { output ->
            ZipOutputStream(output).use { zip ->
                (resources + (PLUGIN_MANIFEST_FILE to manifest.toJson().toByteArray())).forEach { (name, content) ->
                    zip.putNextEntry(ZipEntry("wrapper/$name"))
                    zip.write(content)
                    zip.closeEntry()
                }
            }
        }.toByteArray()
        return PluginZipArchive.read(bytes.inputStream())
    }
    fun importArchive(archive: PluginZipArchive, canReplace: (String) -> Boolean = { true }) =
        importer.importPackage(ImportedPluginPackage(archive.manifest,
            archive.files.mapValues { decodePluginUtf8(it.value) }), canReplace)
    val zipped = zipPackage(original.copy(id = "zip-example"), code)
    val zipInstalled = File(store, "zip-example")
    check(importArchive(zipped).manifest.id == "zip-example")
    check(File(zipInstalled, "entry.lua").exists() == ("entry.lua" in code))
    check(directorySnapshot(zipInstalled).keys == code.keys + PLUGIN_MANIFEST_FILE)
    val zipBefore = directorySnapshot(zipInstalled)
    expectImportFailure("ZIP active replacement") { importArchive(zipped) { false } }
    expectImportFailure("ZIP missing resource") { zipPackage(original.copy(id = "zip-example"), code - original.entry) }
    expectImportFailure("ZIP invalid decoded source map") {
        importer.importPackage(ImportedPluginPackage(zipped.manifest, emptyMap()))
    }
    check(directorySnapshot(zipInstalled) == zipBefore)
    noTransactionDirectories(store)
    zipInstalled.deleteRecursively()
    val nativeOnly = File(store, "native-only").apply { mkdir() }
    File(nativeOnly, PLUGIN_MANIFEST_FILE).writeText(PluginPackageManifest(
        "native-only", "Native", "1", entry = "lib.so", files = listOf("lib.so"), kind = "native",
    ).toJson())
    File(nativeOnly, "lib.so").writeBytes(byteArrayOf(0x7f, 0x45, 0x4c, 0x46))
    val nativeBefore = directorySnapshot(nativeOnly)
    expectImportFailure("ZIP cannot replace native package with Lua") {
        importArchive(zipPackage(original.copy(id = "native-only"), code))
    }
    check(directorySnapshot(nativeOnly) == nativeBefore)
    nativeOnly.deleteRecursively()
    println("OK: ZIP decoding installs only declared Lua resources; failed ZIP imports and kind collisions preserve originals")

    val nested = PluginPackageManifest(
        id = "nested", name = "Nested fixture", version = "1", entry = "entry.lua",
        files = listOf("entry.lua", "lib/hook.c"),
    )
    val nestedCode = mapOf(
        "entry.lua" to "error('IMPORT_MUST_NOT_EXECUTE')".toByteArray(),
        "lib/hook.c" to ("/*" + "x".repeat(180 * 1024) + "*/").toByteArray(),
    )
    importer.importFolder(provider.tree(providerFiles(nested, nestedCode)))
    check(importer.list().map { it.manifest.id }.toSet() == setOf("example-plugin", "nested"))
    check(File(store, "nested/lib/hook.c").readBytes().contentEquals(nestedCode.getValue("lib/hook.c")))
    check(Files.getPosixFilePermissions(File(store, "nested/lib").toPath()) == PosixFilePermissions.fromString("rwx------"))
    println("OK: nested provider traversal and 180 KiB resources stream without argv or code execution")

    val broken = File(store, "broken").apply { mkdir() }
    File(broken, PLUGIN_MANIFEST_FILE).writeText("{bad manifest")
    check(importer.list().map { it.manifest.id }.toSet() == setOf("example-plugin", "nested"))
    check(importer.lastScanErrors.any { it.startsWith("broken:") })
    println("OK: one corrupt stored package does not hide usable imports")

    val replacement = original.copy(version = "test-replacement")
    expectImportFailure("missing declared source") {
        importer.importFolder(provider.tree(providerFiles(replacement, code - "helper.lua")))
    }
    check(directorySnapshot(installed) == originalDisk)
    expectImportFailure("oversized declared source") {
        importer.importFolder(provider.tree(providerFiles(replacement,
            code + ("hook.c" to ByteArray(MAX_PLUGIN_PACKAGE_BYTES + 1) { 120 }))))
    }
    check(directorySnapshot(installed) == originalDisk)
    expectImportFailure("invalid UTF-8 source") {
        importer.importFolder(provider.tree(providerFiles(replacement,
            code + ("hook.c" to byteArrayOf(0xc3.toByte(), 0x28)))))
    }
    check(directorySnapshot(installed) == originalDisk)
    val pageReplacement = replacement.copy(ui = "ui.json", files = replacement.files + "ui.json")
    val invalidPage = """{"version":1,"title":"Status","blocks":[{"type":"button","action":"exec","label":"Run"}]}"""
    expectImportFailure("invalid plugin page") {
        importer.importFolder(provider.tree(providerFiles(pageReplacement, code + ("ui.json" to invalidPage.toByteArray()))))
    }
    check(directorySnapshot(installed) == originalDisk)
    expectImportFailure("non-tree URI") { importer.importFolder(uri.copy(tree = false)) }
    noTransactionDirectories(store)
    println("OK: missing/oversized/invalid resources and plugin pages preserve the existing package")

    var checks = 0
    expectImportFailure("active replacement") {
        importer.importFolder(provider.tree(providerFiles(replacement))) { id ->
            check(id == "example-plugin")
            checks++
            false
        }
    }
    check(checks == 1 && directorySnapshot(installed) == originalDisk)
    checks = 0
    expectImportFailure("activation during staging") {
        importer.importFolder(provider.tree(providerFiles(replacement))) { id ->
            check(id == "example-plugin")
            ++checks < 2
        }
    }
    check(checks == 2 && directorySnapshot(installed) == originalDisk)
    noTransactionDirectories(store)
    println("OK: replacement guards before and after staging preserve the old package")

    val page = """{"version":1,"title":"Status","blocks":[{"type":"status","bind":"runtime.state"}]}"""
    val replaced = importer.importFolder(provider.tree(providerFiles(pageReplacement, code + ("ui.json" to page.toByteArray()))))
    check(replaced.manifest == pageReplacement && replaced.page?.blocks == listOf(PluginPageBlock.Status))
    check(importer.list().single { it.manifest.id == "example-plugin" }.page == replaced.page)
    val replacedDisk = directorySnapshot(installed)
    val commitFailure = File(sandbox, "fail-next-commit").apply { writeText("once") }
    val failure = expectImportFailure("injected staging rename failure") {
        importer.importFolder(provider.tree(providerFiles(original.copy(version = "must-not-commit"))))
    }
    check(!commitFailure.exists() && failure.message.orEmpty().contains("injected staging commit failure"))
    check(directorySnapshot(installed) == replacedDisk)
    noTransactionDirectories(store)
    println("OK: successful replacement commits and failed final rename restores the prior version")

    fun base64Reads(file: File): Int = File(sandbox, "audit.jsonl").readLines().count { line ->
        val event = JSONObject(line)
        event.getString("tool") == "base64" && event.getJSONArray("args").getString(0) == file.absolutePath
    }
    val hook = File(installed, "hook.c")
    val hookBytes = hook.readBytes()
    val target = File(sandbox, "mapped-data/adb/dejavu/contained-symlink-target.c").apply { writeBytes(hookBytes) }
    check(hook.delete())
    Files.createSymbolicLink(hook.toPath(), target.toPath())
    val beforeReads = base64Reads(hook)
    check(importer.list().none { it.manifest.id == "example-plugin" })
    check(importer.lastScanErrors.any { it.startsWith("example-plugin:") })
    check(base64Reads(hook) == beforeReads) { "Production importer read through a stored source symlink" }
    Files.delete(hook.toPath())
    hook.writeBytes(hookBytes)

    val held = File(sandbox, "mapped-data/adb/dejavu/held-example-plugin")
    Files.move(installed.toPath(), held.toPath())
    Files.createSymbolicLink(installed.toPath(), held.toPath())
    check(importer.list().none { it.manifest.id == "example-plugin" })
    expectImportFailure("symlink package destination") {
        importer.importFolder(provider.tree(providerFiles(original)))
    }
    check(directorySnapshot(held) == replacedDisk && Files.isSymbolicLink(installed.toPath()))
    noTransactionDirectories(store)
    println("OK: stored resource and package-directory symlinks are rejected without touching their targets")
    println("OK: importer host regression completed entirely inside ${sandbox.name}")
}
