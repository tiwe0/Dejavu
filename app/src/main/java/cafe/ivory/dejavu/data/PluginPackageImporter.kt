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
        val errors = mutableListOf<String>()
        val result = RootShell.run("""
            if [ ! -e ${q(DEFAULT_PLUGIN_DIRECTORY)} ] && [ ! -L ${q(DEFAULT_PLUGIN_DIRECTORY)} ]; then exit 0; fi
            ${directoryGuard(DEFAULT_PLUGIN_DIRECTORY)} || exit 1
            for path in ${q(DEFAULT_PLUGIN_DIRECTORY)}/*; do
                [ -d "${'$'}path" ] && [ ! -L "${'$'}path" ] || continue
                [ -f "${'$'}path/$PLUGIN_MANIFEST_FILE" ] || continue
                printf '%s\n' "${'$'}{path##*/}"
            done
            exit 0
        """.trimIndent())
        if (result.exitCode != 0) {
            lastScanErrors = listOf("Unable to scan $DEFAULT_PLUGIN_DIRECTORY: ${result.output.trim()}")
            return@synchronized emptyList()
        }
        val packages = result.output.lineSequence().filter { it.isNotBlank() }.mapNotNull { id ->
            try {
                validatePluginId(id)
                readStoredPackage(id)
            } catch (error: Exception) {
                errors += "$id: ${error.message ?: "invalid package"}"
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
        val pkg = ImportedPluginPackage(manifest, sources).also { it.validate() }
        // Validate the generated transport size before touching an existing import.
        LuaPackageScripts.start(pkg, "0000000000000000")
        synchronized(storageLock) {
            require(canReplace(manifest.id)) { "Stop this plugin before replacing its imported sources" }
            store(pkg, canReplace)
        }
        return pkg
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

    private fun readStoredPackage(id: String): ImportedPluginPackage? {
        val directory = "$DEFAULT_PLUGIN_DIRECTORY/$id"
        val manifestBytes = readRootFile("$directory/$PLUGIN_MANIFEST_FILE", MAX_PLUGIN_MANIFEST_BYTES)
        val manifest = PluginPackageManifest.parse(decodePluginUtf8(manifestBytes))
        require(manifest.id == id) { "Plugin identifier does not match its directory" }
        if (manifest.kind != "lua") return null
        var remaining = MAX_PLUGIN_PACKAGE_BYTES - manifestBytes.size
        val sources = manifest.files.associateWith { path ->
            val bytes = readRootFile("$directory/$path", remaining)
            remaining -= bytes.size
            decodePluginUtf8(bytes)
        }
        return ImportedPluginPackage(manifest, sources).also {
            it.validate()
            LuaPackageScripts.start(it, "0000000000000000")
        }
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
            require(canReplace(id)) { "Stop this plugin before replacing its imported sources" }
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
