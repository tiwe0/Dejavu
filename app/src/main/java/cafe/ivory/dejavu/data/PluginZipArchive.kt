package cafe.ivory.dejavu.data

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.util.zip.ZipInputStream

/** A fully checked, in-memory package. Only manifest-declared resources are retained. */
data class PluginZipArchive(val manifest: PluginPackageManifest, val files: Map<String, ByteArray>) {
    companion object {
        private const val MAX_COMPRESSED_BYTES = 80 * 1024 * 1024
        private const val MAX_EXPANDED_BYTES = 64 * 1024 * 1024
        private const val MAX_ENTRIES = 1024
        private const val CENTRAL_HEADER = 0x02014b50L
        private const val LOCAL_HEADER = 0x04034b50L
        private const val END_HEADER = 0x06054b50L
        private const val DESCRIPTOR_HEADER = 0x08074b50L

        fun read(input: InputStream): PluginZipArchive {
            val bytes = readBounded(input, MAX_COMPRESSED_BYTES)
            val entries = directory(bytes)
            val manifests = entries.filter { !it.directory && it.name.substringAfterLast('/') == PLUGIN_MANIFEST_FILE }
            require(manifests.size == 1) { "ZIP must contain exactly one manifest.json" }
            val manifestEntry = manifests.single()
            val prefix = manifestEntry.name.substringBeforeLast('/', "")
            require(prefix.isEmpty() || '/' !in prefix) { "manifest.json must be at ZIP root or inside one plugin folder" }
            val manifestBytes = extract(bytes, manifestEntry, MAX_PLUGIN_MANIFEST_BYTES)
            val manifest = PluginPackageManifest.parse(decodePluginUtf8(manifestBytes))
            val resourcePrefix = if (prefix.isEmpty()) "" else "$prefix/"
            val expected = manifest.files.associateBy { resourcePrefix + it }
            require(expected.keys.all { path -> entries.any { !it.directory && it.name == path } }) {
                "ZIP is missing a manifest-declared resource"
            }
            val files = linkedMapOf<String, ByteArray>()
            var total = 0L
            ZipInputStream(ByteArrayInputStream(bytes)).use { zip ->
                for (record in entries.sortedBy { it.offset }) {
                    val entry = zip.nextEntry ?: throw IllegalArgumentException("Truncated ZIP entry")
                    require(entry.name == record.name && entry.isDirectory == record.directory) { "ZIP entry mismatch" }
                    val path = expected[record.name]
                    val limit = when {
                        record.name == manifestEntry.name -> MAX_PLUGIN_MANIFEST_BYTES
                        path == null -> MAX_EXPANDED_BYTES
                        manifest.kind == "lua" -> MAX_PLUGIN_PACKAGE_BYTES
                        path.endsWith(".c") -> 512 * 1024
                        else -> 32 * 1024 * 1024
                    }
                    val retained = if (path != null) ByteArrayOutputStream() else null
                    val buffer = ByteArray(8192)
                    var size = 0L
                    while (true) {
                        val count = zip.read(buffer)
                        if (count < 0) break
                        size += count
                        total += count
                        require(size <= limit && total <= MAX_EXPANDED_BYTES) { "ZIP exceeds its expanded size limit" }
                        retained?.write(buffer, 0, count)
                    }
                    require(size == record.size && (!record.directory || size == 0L)) { "ZIP entry size mismatch" }
                    if (path != null) files[path] = requireNotNull(retained).toByteArray()
                    zip.closeEntry() // ZipInputStream verifies CRC while draining each entry.
                }
                require(zip.nextEntry == null) { "Unexpected ZIP entry" }
            }
            require(files.keys == manifest.files.toSet()) { "ZIP resources do not match the manifest" }
            val packageSize = manifestBytes.size.toLong() + files.values.sumOf { it.size.toLong() }
            require(packageSize <= if (manifest.kind == "lua") MAX_PLUGIN_PACKAGE_BYTES else MAX_EXPANDED_BYTES) {
                "Plugin package exceeds its size limit"
            }
            if (manifest.kind == "lua") {
                ImportedPluginPackage(manifest, files.mapValues { decodePluginUtf8(it.value) }).validate()
            }
            return PluginZipArchive(manifest, files)
        }

        private data class Entry(
            val name: String,
            val directory: Boolean,
            val offset: Int,
            val size: Long,
            val compressedSize: Long,
            val crc: Long,
            val flags: Int,
            val method: Int,
        )

        /** Check complete central/local structure: ZipInputStream alone accepts a missing central directory. */
        private fun directory(bytes: ByteArray): List<Entry> {
            require(bytes.size >= 22 && u32(bytes, 0) == LOCAL_HEADER) { "Choose a ZIP plugin package" }
            val end = (bytes.size - 22 downTo maxOf(0, bytes.size - 22 - 65535)).firstOrNull {
                u32(bytes, it) == END_HEADER && it + 22 + u16(bytes, it + 20) == bytes.size
            } ?: throw IllegalArgumentException("ZIP central directory is missing or truncated")
            val count = u16(bytes, end + 10)
            require(u16(bytes, end + 4) == 0 && u16(bytes, end + 6) == 0 &&
                u16(bytes, end + 8) == count && count in 1..MAX_ENTRIES) { "Unsupported ZIP disk or entry count" }
            val centralSize = u32(bytes, end + 12)
            val centralOffset = u32(bytes, end + 16)
            require(centralOffset + centralSize == end.toLong() && centralOffset <= Int.MAX_VALUE) {
                "Invalid ZIP central directory bounds"
            }
            var cursor = centralOffset.toInt()
            val result = ArrayList<Entry>(count)
            var expanded = 0L
            repeat(count) {
                require(cursor.toLong() + 46 <= end && u32(bytes, cursor) == CENTRAL_HEADER) { "Invalid ZIP central entry" }
                val flags = u16(bytes, cursor + 8)
                val method = u16(bytes, cursor + 10)
                require(flags and 0xF7F1 == 0 && method in setOf(0, 8) && (method != 0 || flags and 8 == 0)) {
                    "Encrypted or unsupported ZIP entry"
                }
                val nameSize = u16(bytes, cursor + 28)
                val extraSize = u16(bytes, cursor + 30)
                val commentSize = u16(bytes, cursor + 32)
                val next = cursor.toLong() + 46 + nameSize + extraSize + commentSize
                require(next <= end && u16(bytes, cursor + 34) == 0) { "Invalid ZIP central entry bounds" }
                val name = decodePluginUtf8(bytes.copyOfRange(cursor + 46, cursor + 46 + nameSize))
                val isDirectory = name.endsWith('/')
                val path = name.removeSuffix("/")
                validateArchivePath(path)
                val unixMode = (u32(bytes, cursor + 38) ushr 16).toInt() and 0xF000
                require(unixMode == 0 || unixMode == (if (isDirectory) 0x4000 else 0x8000)) {
                    "ZIP links and special files are unsupported"
                }
                val size = u32(bytes, cursor + 24)
                val compressed = u32(bytes, cursor + 20)
                val localOffset = u32(bytes, cursor + 42)
                require(size <= MAX_EXPANDED_BYTES && compressed <= MAX_COMPRESSED_BYTES && localOffset < centralOffset) {
                    "ZIP entry exceeds its size limit"
                }
                expanded += size
                require(expanded <= MAX_EXPANDED_BYTES && (!isDirectory || size == 0L)) { "ZIP exceeds its expanded size limit" }
                result += Entry(name, isDirectory, localOffset.toInt(), size, compressed,
                    u32(bytes, cursor + 16), flags, method)
                cursor = next.toInt()
            }
            require(cursor == end) { "ZIP central directory size mismatch" }
            val paths = result.map { it.name.removeSuffix("/") }
            require(paths.distinct().size == paths.size) { "ZIP has duplicate resource paths" }
            val filePaths = result.filterNot { it.directory }.map { it.name }.toSet()
            require(paths.none { path -> path.split('/').dropLast(1).runningFold("") { parent, part ->
                if (parent.isEmpty()) part else "$parent/$part"
            }.any { it in filePaths } }) { "ZIP paths conflict as files and directories" }
            val sorted = result.sortedBy { it.offset }
            var expectedOffset = 0L
            for ((index, record) in sorted.withIndex()) {
                val offset = record.offset
                require(offset.toLong() == expectedOffset && offset.toLong() + 30 <= centralOffset &&
                    u32(bytes, offset) == LOCAL_HEADER) { "Invalid ZIP local entry offset" }
                require(u16(bytes, offset + 6) == record.flags && u16(bytes, offset + 8) == record.method) {
                    "ZIP local entry does not match its directory"
                }
                val nameSize = u16(bytes, offset + 26)
                val dataOffset = offset.toLong() + 30 + nameSize + u16(bytes, offset + 28)
                val dataEnd = dataOffset + record.compressedSize
                val nextOffset = sorted.getOrNull(index + 1)?.offset?.toLong() ?: centralOffset
                require(dataOffset <= nextOffset && dataEnd <= nextOffset) { "Truncated ZIP entry data" }
                require(decodePluginUtf8(bytes.copyOfRange(offset + 30, offset + 30 + nameSize)) == record.name) {
                    "ZIP local filename does not match its directory"
                }
                if (record.flags and 8 == 0) {
                    require(dataEnd == nextOffset && u32(bytes, offset + 14) == record.crc &&
                        u32(bytes, offset + 18) == record.compressedSize && u32(bytes, offset + 22) == record.size) {
                        "ZIP local sizes or checksum do not match its directory"
                    }
                } else {
                    val length = nextOffset - dataEnd
                    require(length == 12L || length == 16L) { "Invalid ZIP data descriptor" }
                    val descriptor = dataEnd.toInt() + if (length == 16L) 4 else 0
                    require((length != 16L || u32(bytes, dataEnd.toInt()) == DESCRIPTOR_HEADER) &&
                        u32(bytes, descriptor) == record.crc && u32(bytes, descriptor + 4) == record.compressedSize &&
                        u32(bytes, descriptor + 8) == record.size) { "ZIP data descriptor mismatch" }
                }
                expectedOffset = nextOffset
            }
            return result
        }

        private fun validateArchivePath(path: String) {
            require(path.length in 1..512 && '\\' !in path && !Regex("^[A-Za-z]:").containsMatchIn(path) &&
                path.none { it.code < 32 || it.code == 127 } &&
                path.split('/').none { it.isEmpty() || it == "." || it == ".." }) { "Unsafe ZIP path: $path" }
        }

        private fun extract(bytes: ByteArray, entry: Entry, limit: Int): ByteArray {
            require(entry.size <= limit) { "Plugin manifest is too large" }
            return ZipInputStream(ByteArrayInputStream(bytes, entry.offset, bytes.size - entry.offset)).use { zip ->
                require(zip.nextEntry?.name == entry.name) { "ZIP manifest entry mismatch" }
                readBounded(zip, limit)
            }
        }

        private fun readBounded(input: InputStream, limit: Int): ByteArray {
            val output = ByteArrayOutputStream()
            val buffer = ByteArray(8192)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                require(output.size().toLong() + count <= limit) { "ZIP exceeds its size limit" }
                output.write(buffer, 0, count)
            }
            return output.toByteArray()
        }

        private fun u16(bytes: ByteArray, offset: Int): Int {
            require(offset >= 0 && offset.toLong() + 2 <= bytes.size) { "Truncated ZIP header" }
            return (bytes[offset].toInt() and 255) or ((bytes[offset + 1].toInt() and 255) shl 8)
        }

        private fun u32(bytes: ByteArray, offset: Int): Long =
            u16(bytes, offset).toLong() or (u16(bytes, offset + 2).toLong() shl 16)
    }
}
