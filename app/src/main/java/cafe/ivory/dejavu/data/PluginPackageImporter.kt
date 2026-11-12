package cafe.ivory.dejavu.data

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.util.Base64
import java.util.UUID

const val DEFAULT_PLUGIN_DIRECTORY = "/data/adb/dejavu/plugins"

/** Imports declared source resources. No package code executes during import. */
class PluginPackageImporter(context: Context) {
    private val resolver = context.applicationContext.contentResolver

    @Volatile
    var lastScanErrors: List<String> = emptyList()
        private set

    fun list(): List<ImportedPluginPackage> = synchronized(storageLock) {
        list(PluginLibraryScan.read())
    }

    internal fun list(scan: PluginLibraryScan): List<ImportedPluginPackage> = synchronized(storageLock) {
        val errors = scan.errors.toMutableList()
        val manifests = scan.manifests.filter { it.manifest.kind == "lua" }
        val checks = manifests.associate { stored ->
            val manifest = stored.manifest
            manifest.id to buildString {
                appendLine("dejavu_remaining=${MAX_PLUGIN_PACKAGE_BYTES - stored.byteSize}")
                manifest.files.forEach { path ->
                    val resource = q("$DEFAULT_PLUGIN_DIRECTORY/${manifest.id}/$path")
                    appendLine("scan_plugin_file $resource \"\${dejavu_remaining}\" || exit 1")
                    appendLine("dejavu_remaining=\$((dejavu_remaining - dejavu_size))")
                    appendLine("base64 $resource || { scan_plugin_error $resource 'cannot read resource'; exit 1; }")
                    appendLine("printf '.\\n'")
                }
            }
        }
        val results = try {
            runPluginScanChecks(checks)
        } catch (error: Exception) {
            lastScanErrors = errors + "Unable to scan Lua resources: ${error.message}"
            return@synchronized emptyList()
        }
        val packages = manifests.mapNotNull { stored ->
            val manifest = stored.manifest
            try {
                val encoded = results.getValue(manifest.id).getOrThrow().split('.')
                require(encoded.last().isBlank() && encoded.size == manifest.files.size + 1) {
                    "Incomplete stored plugin sources"
                }
                var remaining = MAX_PLUGIN_PACKAGE_BYTES - stored.byteSize
                val sources = manifest.files.mapIndexed { index, path ->
                    val bytes = decodePluginScanBytes(encoded[index])
                    require(bytes.size <= remaining) { "$path exceeds the stored package size limit" }
                    remaining -= bytes.size
                    val source = try {
                        decodePluginUtf8(bytes)
                    } catch (error: Exception) {
                        throw IOException("Invalid UTF-8 resource: $path", error)
                    }
                    path to source
                }.toMap()
                ImportedPluginPackage(manifest, sources).also {
                    it.validate()
                    LuaPackageScripts.start(it, "0000000000000000")
                }
            } catch (error: Exception) {
                errors += "${manifest.id}: ${error.message ?: "invalid package"}"
                null
            }
        }.sortedBy { it.manifest.name.lowercase() }.toList()
        lastScanErrors = errors.toList()
        packages
    }

    fun importFolder(
        uri: Uri,
        canReplace: (String) -> Boolean = { true },
    ): ImportedPluginPackage {
        require(DocumentsContract.isTreeUri(uri)) { "Choose the plugin folder using the folder picker" }
        val root = DocumentsContract.buildDocumentUriUsingTree(uri, DocumentsContract.getTreeDocumentId(uri))
        val manifestBytes = readDocument(resolve(uri, root, PLUGIN_MANIFEST_FILE), MAX_PLUGIN_MANIFEST_BYTES)
        val manifest = PluginPackageManifest.parse(decodePluginUtf8(manifestBytes))
        require(manifest.kind == "lua") { "Import native/JIT packages with the native plugin importer" }
        var remaining = MAX_PLUGIN_PACKAGE_BYTES - manifestBytes.size
        val sources = manifest.files.associateWith { path ->
            val bytes = readDocument(resolve(uri, root, path), remaining)
            remaining -= bytes.size
            decodePluginUtf8(bytes)
        }
        return importPackage(ImportedPluginPackage(manifest, sources), canReplace)
    }

    /** Accepts an already decoded ZIP package through the same atomic storage path. */
    fun importPackage(
        pkg: ImportedPluginPackage,
        canReplace: (String) -> Boolean = { true },
    ): ImportedPluginPackage {
        pkg.validate()
        // Validate the generated transport size before touching an existing import.
        LuaPackageScripts.start(pkg, "0000000000000000")
        synchronized(storageLock) {
            checkReplacement(pkg.manifest.id, canReplace)
            store(pkg, canReplace)
        }
        return pkg
    }

    private fun checkReplacement(id: String, canReplace: (String) -> Boolean) {
        require(canReplace(id)) { "Stop this plugin before replacing its imported sources" }
        val directory = "$DEFAULT_PLUGIN_DIRECTORY/$id"
        val exists = RootShell.run("if [ -e ${q(directory)} ] || [ -L ${q(directory)} ]; then printf existing; fi")
        check(exists.exitCode == 0) { "Unable to inspect the existing plugin" }
        if (exists.output.isNotEmpty()) {
            val previous = PluginPackageManifest.parse(decodePluginUtf8(
                readRootFile("$directory/$PLUGIN_MANIFEST_FILE", MAX_PLUGIN_MANIFEST_BYTES),
            ))
            require(previous.id == id && previous.kind == "lua") {
                "Cannot replace a different plugin kind; use a different plugin identifier"
            }
        }
    }

    private fun resolve(tree: Uri, root: Uri, relativePath: String): Uri {
        if (relativePath != PLUGIN_MANIFEST_FILE) validatePluginPath(relativePath)
        var parent = root
        val components = relativePath.split('/')
        components.forEachIndexed { index, component ->
            val children = DocumentsContract.buildChildDocumentsUriUsingTree(
                tree, DocumentsContract.getDocumentId(parent),
            )
            var found: Uri? = null
            var seen = 0
            resolver.query(children, arrayOf(
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
            ), null, null, null)?.use { cursor ->
                while (cursor.moveToNext()) {
                    require(++seen <= 4096) { "Plugin folder contains too many entries" }
                    if (cursor.getString(1) != component) continue
                    require(found == null) { "Ambiguous plugin resource: $relativePath" }
                    val directory = cursor.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR
                    require(directory == (index < components.lastIndex)) {
                        "Plugin resource has the wrong file type: $relativePath"
                    }
                    found = DocumentsContract.buildDocumentUriUsingTree(tree, cursor.getString(0))
                }
            } ?: throw IOException("Unable to read the selected plugin folder")
            parent = found ?: throw IOException("Missing declared plugin resource: $relativePath")
        }
        return parent
    }

    private fun readDocument(uri: Uri, limit: Int): ByteArray {
        return resolver.openInputStream(uri)?.use { input ->
            val output = ByteArrayOutputStream()
            val buffer = ByteArray(8192)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                require(output.size().toLong() + count <= limit) { "Plugin package exceeds its size limit" }
                output.write(buffer, 0, count)
            }
            output.toByteArray()
        } ?: throw IOException("Unable to open a declared plugin resource")
    }

    private fun readRootFile(path: String, limit: Int): ByteArray {
        val parent = path.substringBeforeLast('/')
        val result = RootShell.run("""
            ${directoryGuard(parent)} &&
            [ ! -L ${q(path)} ] && [ -f ${q(path)} ] &&
            [ "${'$'}(wc -c < ${q(path)})" -le $limit ] && base64 ${q(path)}
        """.trimIndent())
        if (result.exitCode != 0) throw IOException("Unable to read plugin resource: ${path.substringAfter(DEFAULT_PLUGIN_DIRECTORY)}")
        val bytes = Base64.getDecoder().decode(result.output.filterNot { it == '\r' || it == '\n' })
        require(bytes.size <= limit) { "Stored plugin package exceeds its size limit" }
        return bytes
    }

    private fun store(pkg: ImportedPluginPackage, canReplace: (String) -> Boolean) {
        val id = pkg.manifest.id
        val token = UUID.randomUUID().toString()
        val stage = "$DEFAULT_PLUGIN_DIRECTORY/.import-$id-$token"
        val backup = "$DEFAULT_PLUGIN_DIRECTORY/.backup-$id-$token"
        val destination = "$DEFAULT_PLUGIN_DIRECTORY/$id"
        checkRoot("""
            umask 077
            [ ! -L /data ] && [ ! -L /data/adb ] || exit 1
            for directory in /data/adb/dejavu ${q(DEFAULT_PLUGIN_DIRECTORY)}; do
                [ ! -L "${'$'}directory" ] || exit 1
                mkdir -p "${'$'}directory" && chown 0:0 "${'$'}directory" && chmod 700 "${'$'}directory" || exit 1
            done
            [ ! -e ${q(stage)} ] && [ ! -L ${q(stage)} ] && mkdir ${q(stage)}
        """.trimIndent())
        try {
            val files = pkg.sources.mapValues { pluginUtf8Bytes(it.value) } +
                (PLUGIN_MANIFEST_FILE to pluginUtf8Bytes(pkg.manifest.toJson()))
            for ((path, bytes) in files) {
                val target = "$stage/$path"
                val parent = target.substringBeforeLast('/')
                checkRoot("""
                    umask 077
                    mkdir -p ${q(parent)} && ${directoryGuard(parent)} &&
                    cat > ${q(target)} && [ "${'$'}(wc -c < ${q(target)})" -eq ${bytes.size} ] &&
                    chmod 600 ${q(target)}
                """.trimIndent(), bytes)
            }
            checkReplacement(id, canReplace)
            checkRoot("""
                ${directoryGuard(DEFAULT_PLUGIN_DIRECTORY)} || exit 1
                [ ! -L ${q(destination)} ] && [ ! -L ${q(backup)} ] && [ ! -e ${q(backup)} ] || exit 1
                rollback() {
                    if [ -d ${q(backup)} ] && [ ! -L ${q(backup)} ] &&
                       [ ! -e ${q(destination)} ] && [ ! -L ${q(destination)} ]; then
                        mv ${q(backup)} ${q(destination)}
                    fi
                }
                trap rollback EXIT
                if [ -e ${q(destination)} ]; then
                    [ -d ${q(destination)} ] && mv ${q(destination)} ${q(backup)} || exit 1
                fi
                mv ${q(stage)} ${q(destination)} || exit 1
            """.trimIndent())
            // The replacement has committed. A backup cleanup error must not be
            // reported as a failed import after the new package is installed.
            val cleanup = RootShell.run("rm -rf ${q(backup)}")
            if (cleanup.exitCode != 0) {
                lastScanErrors = listOf("Imported $id, but could not remove its previous backup")
            }
        } finally {
            RootShell.run("rm -rf ${q(stage)}")
        }
    }

    private fun checkRoot(command: String, input: ByteArray? = null) {
        val result = if (input == null) RootShell.run(command) else RootShell.run(command, input)
        if (result.exitCode != 0) throw IOException("Unable to store plugin package: ${result.output.trim()}")
    }

    companion object {
        private val storageLock = Any()
        private fun q(value: String) = RootShell.quote(value)

        private fun directoryGuard(path: String): String {
            val components = path.split('/').filter { it.isNotEmpty() }
            return components.indices.joinToString(" && ") { index ->
                val directory = "/" + components.take(index + 1).joinToString("/")
                "[ -d ${q(directory)} ] && [ ! -L ${q(directory)} ]"
            }
        }
    }
}
