package cafe.ivory.dejavu.data

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.util.zip.CRC32
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

private fun zip(files: List<Pair<String, ByteArray>>, stored: Boolean = false): ByteArray {
    val output = ByteArrayOutputStream()
    ZipOutputStream(output).use { zip ->
        for ((name, bytes) in files) {
            val entry = ZipEntry(name)
            if (stored) {
                entry.method = ZipEntry.STORED
                entry.size = bytes.size.toLong()
                entry.compressedSize = bytes.size.toLong()
                entry.crc = CRC32().apply { update(bytes) }.value
            }
            zip.putNextEntry(entry)
            zip.write(bytes)
            zip.closeEntry()
        }
    }
    return output.toByteArray()
}

private fun text(value: String) = value.toByteArray(Charsets.UTF_8)
private fun read(bytes: ByteArray) = PluginZipArchive.read(ByteArrayInputStream(bytes))
private fun reject(name: String, bytes: ByteArray) {
    check(runCatching { read(bytes) }.isFailure) { "Accepted invalid ZIP: $name" }
}
private fun manifest(kind: String = "lua", entry: String = "app.lua", files: List<String> = listOf(entry), ui: String? = null) =
    text(PluginPackageManifest("example", "Example", "1", entry = entry, files = files, kind = kind, ui = ui).toJson())
private fun packageFiles() = listOf("manifest.json" to manifest(), "app.lua" to text("return {}"))
private fun positions(bytes: ByteArray, signature: Long): List<Int> =
    (0..bytes.size - 4).filter { u32(bytes, it) == signature }
private fun u32(bytes: ByteArray, offset: Int): Long = (0..3).fold(0L) { value, i ->
    value or ((bytes[offset + i].toLong() and 255) shl (8 * i))
}
private fun put32(bytes: ByteArray, offset: Int, value: Long) {
    for (i in 0..3) bytes[offset + i] = (value ushr (8 * i)).toByte()
}
private fun replaceName(bytes: ByteArray, from: String, to: String): ByteArray {
    require(from.length == to.length)
    val source = text(from)
    return bytes.copyOf().also { copy ->
        for (i in 0..copy.size - source.size) if (source.indices.all { copy[i + it] == source[it] })
            text(to).copyInto(copy, i)
    }
}

fun main() {
    val plain = zip(packageFiles())
    val parsed = read(plain)
    check(parsed.manifest.id == "example" && parsed.files.keys == setOf("app.lua"))
    check(decodePluginUtf8(parsed.files.getValue("app.lua")) == "return {}")
    check(read(zip(packageFiles(), stored = true)).manifest == parsed.manifest)
    check(read(zip(packageFiles().map { "release-1/" + it.first to it.second })).files.keys == setOf("app.lua"))
    check(read(zip(packageFiles() + listOf("docs/" to byteArrayOf(), "docs/info.txt" to text("unused"),
        "__MACOSX/._manifest.json" to text("finder"), ".DS_Store" to text("finder")))).files.keys == setOf("app.lua"))
    val native = listOf("manifest.json" to manifest("native", "plugin.so"),
        "plugin.so" to byteArrayOf(0x7f, 0x45, 0x4c, 0x46, 0))
    check(read(zip(native)).manifest.kind == "native")
    check(read(zip(listOf("manifest.json" to manifest("jit", "plugin.c"),
        "plugin.c" to text("void dejavu_jit_plugin_init(void) {}")))).manifest.kind == "jit")
    reject("not zip", text("plugin"))
    reject("missing manifest", zip(listOf("app.lua" to text("return {}"))))
    reject("invalid manifest", zip(listOf("manifest.json" to text("{}"), "app.lua" to text("return {}"))))
    reject("invalid utf8 manifest", zip(listOf("manifest.json" to byteArrayOf(0xC3.toByte()), "app.lua" to text("return {}"))))
    reject("multiple manifests", zip(packageFiles() + ("nested/manifest.json" to manifest())))
    reject("deeply nested manifest", zip(packageFiles().map { "outer/inner/" + it.first to it.second }))
    reject("missing resource", zip(listOf("manifest.json" to manifest())))
    reject("directory instead of resource", zip(listOf("manifest.json" to manifest(), "app.lua/" to byteArrayOf())))
    reject("invalid source utf8", zip(listOf("manifest.json" to manifest(), "app.lua" to byteArrayOf(0xC3.toByte()))))
    reject("invalid page", zip(listOf("manifest.json" to manifest(files = listOf("app.lua", "ui.json"), ui = "ui.json"),
        "app.lua" to text("return {}"), "ui.json" to text("{}"))))
    reject("oversize manifest", zip(listOf("manifest.json" to ByteArray(MAX_PLUGIN_MANIFEST_BYTES + 1) { ' '.code.toByte() }, "app.lua" to text("return {}"))))
    for (path in listOf("../bad", "/bad", "a/../bad", "./bad", "a//bad", "C:/bad", "a\\bad", "bad\nname")) {
        reject("unsafe $path", zip(packageFiles() + (path to text("bad"))))
    }
    reject("duplicate filename", replaceName(zip(packageFiles() + listOf("one.lua" to text("one"), "two.lua" to text("two"))), "one.lua", "two.lua"))
    reject("file-dir conflict", zip(packageFiles() + listOf("data" to text("file"), "data/child" to text("child"))))
    reject("explicit file-dir conflict", zip(packageFiles() + listOf("data" to text("file"), "data/" to byteArrayOf())))
    reject("truncated end", plain.copyOf(plain.size - 1))
    reject("missing central directory", plain.copyOf(positions(plain, 0x02014b50L).first()))
    reject("trailing garbage", plain + text("garbage"))
    val central = positions(plain, 0x02014b50L)
    reject("invalid local offset", plain.copyOf().also { put32(it, central.last() + 42, 0) })
    reject("invalid declared size", plain.copyOf().also { put32(it, central.last() + 24, 64L * 1024 * 1024 + 1) })
    reject("encrypted", plain.copyOf().also { it[central.first() + 8] = (it[central.first() + 8].toInt() or 1).toByte() })
    reject("unsupported compression", plain.copyOf().also { it[central.first() + 10] = 99 })
    reject("Unix symlink", plain.copyOf().also { put32(it, central.last() + 38, 0xA1FF0000L) })
    reject("Unix device", plain.copyOf().also { put32(it, central.last() + 38, 0x21FF0000L) })
    val stored = zip(packageFiles(), stored = true)
    val appNameOffset = positions(stored, 0x04034b50L).last()
    reject("bad CRC", stored.copyOf().also { it[appNameOffset + 30 + "app.lua".length] = 'X'.code.toByte() })
    reject("entry count", zip(packageFiles() + (0..1023).map { "extras/$it" to byteArrayOf() }))
    reject("Lua package size", zip(listOf("manifest.json" to manifest(), "app.lua" to ByteArray(MAX_PLUGIN_PACKAGE_BYTES) { 'a'.code.toByte() })))
    // A declared 33 MiB entry is rejected independently from the package's 64 MiB limit.
    reject("native individual resource size", zip(listOf(
        "manifest.json" to manifest("native", "plugin.so", listOf("plugin.so", "large.bin")),
        "plugin.so" to byteArrayOf(0x7f, 0x45, 0x4c, 0x46), "large.bin" to ByteArray(33 * 1024 * 1024))))
    reject("JIT individual size", zip(listOf("manifest.json" to manifest("jit", "plugin.c"),
        "plugin.c" to ByteArray(512 * 1024 + 1) { 'a'.code.toByte() })))
    val forged = zip(listOf("manifest.json" to manifest(), "app.lua" to ByteArray(300 * 1024)))
    val forgedCentral = positions(forged, 0x02014b50L).last()
    val forgedDescriptor = positions(forged, 0x08074b50L).last()
    put32(forged, forgedCentral + 24, 1)
    put32(forged, forgedDescriptor + 12, 1)
    reject("forged small uncompressed size", forged)
    // Central sizes count extras too, preventing a compressed bomb from being inflated at all.
    reject("expanded bomb in extras", zip(packageFiles() + ("huge.bin" to ByteArray(64 * 1024 * 1024))))
    println("PluginZipArchive: Lua/native/JIT, wrapping, manifest/resource validation, ZIP structure/CRC, paths, links and size limits passed")
}
