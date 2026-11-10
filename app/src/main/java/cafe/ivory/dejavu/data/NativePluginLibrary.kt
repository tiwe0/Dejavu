package cafe.ivory.dejavu.data

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

/** Root-owned source library. Scanning and importing never execute plugin code. */
class NativePluginLibrary(context: Context) {
    private val resolver = context.applicationContext.contentResolver
    @Volatile var lastScanErrors: List<String> = emptyList()
        private set

    fun list(): List<NativePluginArtifact> = synchronized(lock) {
        val result = RootShell.run("""
            if [ ! -e ${q(DEFAULT_PLUGIN_DIRECTORY)} ] && [ ! -L ${q(DEFAULT_PLUGIN_DIRECTORY)} ]; then exit 0; fi
            ${directoryGuard(DEFAULT_PLUGIN_DIRECTORY)} || exit 1
            for parent in ${q(DEFAULT_PLUGIN_DIRECTORY)}/*; do
                [ -d "${'$'}parent" ] && [ ! -L "${'$'}parent" ] || continue
                if [ -e "${'$'}parent/manifest.json" ] || [ -L "${'$'}parent/manifest.json" ]; then
                    printf '%s\n' "${'$'}parent/manifest.json"
                fi
            done
        """.trimIndent())
        if (result.exitCode != 0) {
            lastScanErrors = listOf("Unable to scan native plugins: ${result.output.trim()}")
            return@synchronized emptyList()
        }
        val errors = mutableListOf<String>()
        val artifacts = result.output.lineSequence().filter { it.isNotBlank() }.mapNotNull { path ->
            try {
                val text = readManifest(path)
                val manifest = NativePluginManifest.parse(text)
                if (manifest.kind == "lua") null else {
                    val directory = path.substringBeforeLast('/')
                    require(directory.substringAfterLast('/') == manifest.id) { "Plugin identifier does not match its directory" }
                    manifestArtifact(directory, manifest).also { validateStored(it) }
                }
            } catch (error: Exception) {
                errors += "${path.substringBeforeLast('/').substringAfterLast('/')}: ${error.message}"
                null
            }
        }.distinctBy { it.path }.sortedBy { it.name.lowercase() }.toList()
        lastScanErrors = errors
        artifacts
    }

    /** Missing or malformed manifests are errors, never an invitation to execute raw files. */
    fun hasLuaManifest(uri: Uri): Boolean {
        val canonical = documents(uri).singleOrNull { it.name == "manifest.json" }
            ?: throw IOException("Missing manifest.json in the selected plugin folder")
        require(!canonical.directory) { "manifest.json must be a file" }
        val manifest = NativePluginManifest.parse(decodePluginUtf8(readDocument(canonical.uri, 16 * 1024)))
        return manifest.kind == "lua"
    }

    fun importFolder(uri: Uri, canReplace: (String) -> Boolean = { true }): List<NativePluginArtifact> {
        val canonical = documents(uri).singleOrNull { it.name == "manifest.json" }
            ?: throw IOException("Missing manifest.json in the selected plugin folder")
        return importManifestFolder(uri, canonical, canReplace)
    }

    /** Does not read the filesystem; safe for joining the source and runtime lists. */
    fun matchesDeployment(process: String, artifact: NativePluginArtifact, path: String): Boolean =
        targetDirectories(process).any { path == "$it/.dejavu/${artifact.deploymentName}" }

    fun deployedPath(process: String, artifact: NativePluginArtifact): String? =
        targetDirectory(process)?.let { "$it/.dejavu/${artifact.deploymentName}" }

    /** Copy to the selected process's readable files directory and pin its hash. */
    fun deploy(process: String, artifact: NativePluginArtifact): String = synchronized(lock) {
        withTrustLock { deployLocked(process, artifact) }
    }

    private fun deployLocked(process: String, artifact: NativePluginArtifact): String {
        val id = requireNotNull(artifact.id) { "Native plugins require a manifest.json folder" }
        validatePluginId(id)
        val sourceDirectory = "$DEFAULT_PLUGIN_DIRECTORY/$id"
        val manifest = NativePluginManifest.parse(readManifest("$sourceDirectory/manifest.json"))
        require(manifestArtifact(sourceDirectory, manifest) == artifact) { "Native plugin manifest changed; rescan the library" }
        validateStored(artifact)
        val base = targetDirectory(process) ?: throw IOException("Unable to find the target app's files directory")
        val directory = "$base/.dejavu"
        val destination = "$directory/${artifact.deploymentName}"
        require(destination.toByteArray(Charsets.UTF_8).size < 92) { "Target plugin path exceeds Android's property limit" }
        val sourceSkill = skillPath(artifact)
        val hash = fileHash(artifact.path)
        val skillHash = sourceSkill?.let(::fileHash).orEmpty()
        val properties = readProperties()
        val existing = (0 until SLOTS).firstOrNull { properties["$ALLOWED.$it"] == destination }
        val slot = existing ?: (0 until SLOTS).firstOrNull { properties["$ALLOWED.$it"].isNullOrBlank() }
            ?: throw IOException("The trusted plugin list is full (16 slots); remove an unused entry first")
        val stage = "$directory/.stage-${UUID.randomUUID()}"
        var committed = false
        try {
            val skillStage = if (sourceSkill == null) "" else """
                cp ${q(sourceSkill)} ${q("$stage/new-skill")}
                chown 0:0 ${q("$stage/new-skill")}
                chmod 644 ${q("$stage/new-skill")}
                chcon u:object_r:apk_data_file:s0 ${q("$stage/new-skill")}
                [ "${'$'}(sha256sum ${q("$stage/new-skill")} | cut -d ' ' -f 1)" = ${q(skillHash)} ]
            """.trimIndent()
            root("""
                set -e
                ${directoryGuard(base)}
                [ ! -L ${q(directory)} ]
                mkdir -p ${q(directory)}
                chown 0:0 ${q(directory)}
                chmod 755 ${q(directory)}
                restorecon ${q(directory)} >/dev/null 2>&1 || true
                ${directoryGuard(directory)}
                [ ! -e ${q(stage)} ] && [ ! -L ${q(stage)} ]
                (umask 077; mkdir ${q(stage)})
                cp ${q(artifact.path)} ${q("$stage/new-plugin")}
                chown 0:0 ${q("$stage/new-plugin")}
                chmod ${if (artifact.jit) "644" else "755"} ${q("$stage/new-plugin")}
                chcon u:object_r:apk_data_file:s0 ${q("$stage/new-plugin")}
                [ "${'$'}(sha256sum ${q("$stage/new-plugin")} | cut -d ' ' -f 1)" = ${q(hash)} ]
                $skillStage
                [ ! -L ${q(destination)} ] && [ ! -L ${q(destination + ".SKILL.md")} ]
                if [ -e ${q(destination)} ]; then [ -f ${q(destination)} ]; fi
                if [ -e ${q(destination + ".SKILL.md")} ]; then [ -f ${q(destination + ".SKILL.md")} ]; fi
                touch ${q("$stage/publishing")}
                if [ -e ${q(destination)} ]; then
                    touch ${q("$stage/had-plugin")}
                    mv ${q(destination)} ${q("$stage/old-plugin")}
                fi
                if [ -e ${q(destination + ".SKILL.md")} ]; then
                    touch ${q("$stage/had-skill")}
                    mv ${q(destination + ".SKILL.md")} ${q("$stage/old-skill")}
                fi
                mv ${q("$stage/new-plugin")} ${q(destination)}
                ${if (sourceSkill == null) "" else "mv ${q("$stage/new-skill")} ${q(destination + ".SKILL.md")}"}
            """.trimIndent())
            writeSlot(slot, destination, hash, skillHash, properties)
            committed = true
            return destination
        } finally {
            if (!committed) {
                root("""
                    set -e
                    ${directoryGuard(directory)}
                    [ ! -L ${q(stage)} ]
                    if [ -f ${q("$stage/publishing")} ]; then
                        if [ -f ${q("$stage/old-plugin")} ]; then
                            mv ${q("$stage/old-plugin")} ${q(destination)}
                        elif [ ! -e ${q("$stage/had-plugin")} ]; then rm -f ${q(destination)}; fi
                        if [ -f ${q("$stage/old-skill")} ]; then
                            mv ${q("$stage/old-skill")} ${q(destination + ".SKILL.md")}
                        elif [ ! -e ${q("$stage/had-skill")} ]; then rm -f ${q(destination + ".SKILL.md")}; fi
                    fi
                """.trimIndent())
            }
            RootShell.run("rm -rf ${q(stage)}")
        }
    }

    private fun <T> withTrustLock(action: () -> T): T {
        val process = RootShell.start("""
            set -e
            umask 077
            [ ! -L /data ] && [ ! -L /data/adb ] && [ ! -L /data/adb/dejavu ]
            mkdir -p /data/adb/dejavu
            chown 0:0 /data/adb/dejavu
            chmod 700 /data/adb/dejavu
            [ ! -L ${q(TRUST_LOCK)} ]
            exec 9>${q(TRUST_LOCK)}
            chmod 600 ${q(TRUST_LOCK)}
            /system/bin/flock -n 9 9>&9 || { echo 'Plugin trust configuration is busy; retry loading'; exit 73; }
            printf '%s\n' ${q(LOCK_READY)}
            read -r dejavu_release || true
        """.trimIndent())
        val ready = CountDownLatch(1)
        val outcome = AtomicReference("")
        Thread {
            try {
                process.inputStream.bufferedReader().use { reader ->
                    val diagnostics = StringBuilder()
                    while (true) {
                        val line = reader.readLine() ?: break
                        if (line == LOCK_READY) {
                            outcome.set(LOCK_READY)
                            return@use
                        }
                        if (diagnostics.length < 2048) diagnostics.appendLine(line.take(256))
                    }
                    outcome.set(diagnostics.toString().trim())
                }
            } catch (error: Exception) {
                outcome.compareAndSet("", error.message.orEmpty())
            } finally {
                ready.countDown()
            }
        }.apply { isDaemon = true; start() }
        try {
            if (!ready.await(5, TimeUnit.SECONDS)) throw IOException("Timed out acquiring the plugin trust lock")
            if (outcome.get() != LOCK_READY || !process.isAlive) {
                throw IOException(outcome.get().ifBlank { "Unable to acquire the plugin trust lock" })
            }
            return action()
        } finally {
            process.outputStream.close()
            if (!process.waitFor(500, TimeUnit.MILLISECONDS)) {
                process.destroy()
                if (!process.waitFor(500, TimeUnit.MILLISECONDS)) process.destroyForcibly()
            }
        }
    }

    private data class Document(val name: String, val uri: Uri, val directory: Boolean)

    private fun importManifestFolder(
        uri: Uri,
        document: Document,
        canReplace: (String) -> Boolean,
    ): List<NativePluginArtifact> {
        require(!document.directory) { "manifest.json must be a file" }
        val manifest = NativePluginManifest.parse(decodePluginUtf8(readDocument(document.uri, 16 * 1024)))
        require(manifest.kind == "native" || manifest.kind == "jit") { "Import this folder as a Lua package" }
        val resources = linkedMapOf<String, ByteArray>()
        var remaining = 64 * 1024 * 1024 - manifest.toJson().toByteArray(Charsets.UTF_8).size
        for (path in manifest.files) {
            path.split('/').forEach(::validateName)
            require(remaining > 0) { "Native plugin folder exceeds 64 MiB" }
            val bytes = readDocument(resolveDocument(uri, path), minOf(limit(path), remaining))
            remaining -= bytes.size
            resources[path] = bytes
        }
        return importPackage(manifest, resources, canReplace)
    }

    /** Validates ZIP resources before staging any change to the installed library. */
    fun importPackage(
        manifest: NativePluginManifest,
        resources: Map<String, ByteArray>,
        canReplace: (String) -> Boolean = { true },
    ): List<NativePluginArtifact> {
        manifest.validate()
        require(manifest.kind == "native" || manifest.kind == "jit") { "Expected a native or JIT plugin" }
        require(resources.keys == manifest.files.toSet()) { "Imported resources do not match the manifest" }
        val files = linkedMapOf("manifest.json" to pluginUtf8Bytes(manifest.toJson()))
        var total = files.values.sumOf { it.size.toLong() }
        for (path in manifest.files) {
            path.split('/').forEach(::validateName)
            val bytes = resources.getValue(path)
            require(bytes.size <= limit(path)) { "Plugin resource exceeds its size limit: $path" }
            total += bytes.size
            require(total <= 64 * 1024 * 1024) { "Native plugin package exceeds 64 MiB" }
            if (path == manifest.entry) validateBytes(path, bytes)
            files[path] = bytes
        }
        val destination = "$DEFAULT_PLUGIN_DIRECTORY/${manifest.id}"
        val imported = artifact("$destination/${manifest.entry}").copy(
            name = manifest.name, id = manifest.id, version = manifest.version, description = manifest.description,
        )
        synchronized(lock) {
            checkReplacement(destination, manifest, canReplace)
            store(destination, files) { checkReplacement(destination, manifest, canReplace) }
        }
        return listOf(imported)
    }

    private fun checkReplacement(
        destination: String,
        manifest: NativePluginManifest,
        canReplace: (String) -> Boolean,
    ) {
        require(canReplace(manifest.id)) { "Stop this plugin before replacing its imported sources" }
        val existing = root("if [ -e ${q(destination)} ] || [ -L ${q(destination)} ]; then printf existing; fi")
        if (existing.isNotEmpty()) {
            val previous = NativePluginManifest.parse(readManifest("$destination/manifest.json"))
            require(previous.id == manifest.id) { "Existing plugin identifier does not match its directory" }
            require(previous.kind == manifest.kind) {
                "Cannot replace a ${previous.kind} plugin with ${manifest.kind}; use a different plugin identifier"
            }
        }
    }

    private fun resolveDocument(uri: Uri, path: String): Uri {
        var parent = DocumentsContract.buildDocumentUriUsingTree(uri, DocumentsContract.getTreeDocumentId(uri))
        val parts = path.split('/')
        for ((index, name) in parts.withIndex()) {
            val found = documents(uri, parent).singleOrNull { it.name == name }
                ?: throw IOException("Missing declared plugin resource: $path")
            require(found.directory == (index < parts.lastIndex)) { "Plugin resource has the wrong file type: $path" }
            parent = found.uri
        }
        return parent
    }

    private fun documents(uri: Uri, parent: Uri? = null): List<Document> {
        require(DocumentsContract.isTreeUri(uri)) { "Choose a plugin folder" }
        val root = parent ?: DocumentsContract.buildDocumentUriUsingTree(uri, DocumentsContract.getTreeDocumentId(uri))
        val children = DocumentsContract.buildChildDocumentsUriUsingTree(uri, DocumentsContract.getDocumentId(root))
        val result = mutableListOf<Document>()
        resolver.query(children, arrayOf(DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE),
            null, null, null)?.use { cursor ->
            while (cursor.moveToNext()) {
                require(result.size < 4096) { "Plugin folder contains too many entries" }
                val name = cursor.getString(1)
                require(result.none { it.name == name }) { "Ambiguous plugin resource: $name" }
                result += Document(name, DocumentsContract.buildDocumentUriUsingTree(uri, cursor.getString(0)),
                    cursor.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR)
            }
        } ?: throw IOException("Unable to read the selected folder")
        return result
    }

    private fun readDocument(uri: Uri, limit: Int): ByteArray = resolver.openInputStream(uri)?.use { input ->
        val output = ByteArrayOutputStream()
        val buffer = ByteArray(8192)
        while (true) {
            val count = input.read(buffer)
            if (count < 0) break
            require(output.size().toLong() + count <= limit) { "Plugin resource exceeds $limit bytes" }
            output.write(buffer, 0, count)
        }
        output.toByteArray()
    } ?: throw IOException("Unable to open a plugin resource")

    private fun validateStored(artifact: NativePluginArtifact) {
        val check = RootShell.run("""
            ${directoryGuard(artifact.path.substringBeforeLast('/'))} &&
            [ ! -L ${q(artifact.path)} ] && [ -f ${q(artifact.path)} ] &&
            [ "${'$'}(wc -c < ${q(artifact.path)})" -gt 0 ] &&
            [ "${'$'}(wc -c < ${q(artifact.path)})" -le ${limit(artifact.path)} ] &&
            ${if (artifact.jit) "grep -q 'dejavu_jit_plugin_init' ${q(artifact.path)}" else
                "[ \"${'$'}(od -An -tx1 -N4 ${q(artifact.path)} | tr -d ' \\n')\" = 7f454c46 ]"}
        """.trimIndent())
        require(check.exitCode == 0) { "Invalid ${if (artifact.jit) "JIT source" else "ELF library"} or unreadable plugin" }
    }

    private fun skillPath(artifact: NativePluginArtifact): String? {
        val path = artifact.path
        val id = requireNotNull(artifact.id) { "Native plugins require a manifest.json folder" }
        validatePluginId(id)
        val directory = "$DEFAULT_PLUGIN_DIRECTORY/$id"
        val manifest = NativePluginManifest.parse(readManifest("$directory/manifest.json"))
        require(manifest.kind in listOf("native", "jit") && manifest.id == id &&
            "$directory/${manifest.entry}" == path) { "Native plugin manifest changed; rescan the library" }
        val candidates = listOf(manifest.entry + ".SKILL.md", manifest.entry.substringBeforeLast('/', "").let {
            if (it.isEmpty()) "SKILL.md" else "$it/SKILL.md"
        }, "SKILL.md").distinct().filter { it in manifest.files }.map { "$directory/$it" }
        for (candidate in candidates) {
            val result = RootShell.run("[ ! -L ${q(candidate)} ] && [ -f ${q(candidate)} ] && [ \"${'$'}(wc -c < ${q(candidate)})\" -le 262144 ]")
            if (result.exitCode == 0) return candidate
        }
        return null
    }

    private fun fileHash(path: String): String {
        val output = root("sha256sum ${q(path)}").substringBefore(' ').trim()
        require(output.matches(Regex("[0-9a-f]{64}"))) { "Unable to hash the plugin resource" }
        return output
    }

    private fun readManifest(path: String): String {
        val encoded = root("""
            ${directoryGuard(path.substringBeforeLast('/'))} &&
            [ ! -L ${q(path)} ] && [ -f ${q(path)} ] &&
            [ "${'$'}(wc -c < ${q(path)})" -le 16384 ] && base64 ${q(path)}
        """.trimIndent())
        return decodePluginUtf8(java.util.Base64.getDecoder().decode(encoded.filterNot { it == '\r' || it == '\n' }))
    }

    private fun manifestArtifact(directory: String, manifest: NativePluginManifest): NativePluginArtifact {
        require(manifest.kind == "native" || manifest.kind == "jit") { "Unsupported native plugin kind" }
        val checks = manifest.files.joinToString("\n") { path ->
            path.split('/').forEach(::validateName)
            val resource = "$directory/$path"
            """
                ${directoryGuard(resource.substringBeforeLast('/'))}
                [ ! -L ${q(resource)} ] && [ -f ${q(resource)} ]
                dejavu_size=${'$'}(wc -c < ${q(resource)})
                [ "${'$'}dejavu_size" -le ${limit(path)} ]
                dejavu_total=${'$'}((dejavu_total + dejavu_size))
                [ "${'$'}dejavu_total" -le 67108864 ]
            """.trimIndent()
        }
        root("set -e\ndejavu_total=0\n$checks")
        val entry = artifact("$directory/${manifest.entry}")
        return entry.copy(name = manifest.name, id = manifest.id, version = manifest.version, description = manifest.description)
    }

    private fun targetDirectory(process: String): String? {
        val paths = targetDirectories(process)
        val output = root(paths.joinToString("\n") { "if ${directoryGuard(it)}; then printf '%s\\n' ${q(it)}; exit 0; fi" } + "\nexit 0")
        return output.lineSequence().firstOrNull { it in paths }
    }

    private fun readProperties(): Map<String, String> {
        val names = (0 until SLOTS).flatMap { listOf("$ALLOWED.$it", "$HASH.$it", "$SKILL_HASH.$it") }
        return root("for name in ${names.joinToString(" ", transform = ::q)}; do printf '%s=' \"${'$'}name\"; getprop \"${'$'}name\"; done")
            .lineSequence().filter { '=' in it }.associate { it.substringBefore('=') to it.substringAfter('=').trim() }
    }

    private fun writeSlot(slot: Int, path: String, hash: String, skillHash: String, previous: Map<String, String>) {
        val values = linkedMapOf("$ALLOWED.$slot" to path, "$HASH.$slot" to hash, "$SKILL_HASH.$slot" to skillHash)
        val rollback = values.keys.joinToString("; ") { "setprop ${q(it)} ${q(previous[it].orEmpty())}" }
        val commands = listOf("setprop ${q("$ALLOWED.$slot")} ''",
            "setprop ${q("$HASH.$slot")} ${q(hash)}", "setprop ${q("$SKILL_HASH.$slot")} ${q(skillHash)}",
            "setprop ${q("$ALLOWED.$slot")} ${q(path)}")
        val unchanged = values.keys.joinToString("\n") {
            "[ \"${'$'}(getprop ${q(it)})\" = ${q(previous[it].orEmpty())} ] || { echo 'Trusted plugin slot changed; retry loading'; exit 1; }"
        }
        root("set -e\n$unchanged\nrollback() { $rollback; }\ntrap rollback EXIT\n" + commands.joinToString("\n") + "\n" +
            values.entries.joinToString("\n") { "[ \"${'$'}(getprop ${q(it.key)})\" = ${q(it.value)} ]" } + "\ntrap - EXIT")
    }

    private fun store(destination: String, files: Map<String, ByteArray>, beforeCommit: () -> Unit) {
        val stage = "$DEFAULT_PLUGIN_DIRECTORY/.native-import-${UUID.randomUUID()}"
        val backup = "$DEFAULT_PLUGIN_DIRECTORY/.native-backup-${UUID.randomUUID()}"
        root("""
            set -e
            [ ! -L /data ] && [ ! -L /data/adb ]
            for directory in /data/adb/dejavu ${q(DEFAULT_PLUGIN_DIRECTORY)}; do
                [ ! -L "${'$'}directory" ]
                mkdir -p "${'$'}directory"
                chown 0:0 "${'$'}directory"
                chmod 700 "${'$'}directory"
            done
            ${directoryGuard(DEFAULT_PLUGIN_DIRECTORY)}
            umask 077
            mkdir ${q(stage)}
        """.trimIndent())
        try {
            files.forEach { (name, bytes) ->
                val path = "$stage/$name"
                val parent = path.substringBeforeLast('/')
                root("umask 077; mkdir -p ${q(parent)} && ${directoryGuard(parent)} && cat > ${q(path)} && [ \"${'$'}(wc -c < ${q(path)})\" -eq ${bytes.size} ] && chmod 600 ${q(path)}", bytes)
            }
            beforeCommit()
            root("""
                set -e
                ${directoryGuard(DEFAULT_PLUGIN_DIRECTORY)}
                [ ! -L ${q(destination)} ] && [ ! -e ${q(backup)} ] && [ ! -L ${q(backup)} ]
                rollback() {
                    if [ -d ${q(backup)} ] && [ ! -e ${q(destination)} ]; then mv ${q(backup)} ${q(destination)}; fi
                }
                trap rollback EXIT
                if [ -e ${q(destination)} ]; then [ -d ${q(destination)} ]; mv ${q(destination)} ${q(backup)}; fi
                mv ${q(stage)} ${q(destination)}
                trap - EXIT
            """.trimIndent())
            RootShell.run("rm -rf ${q(backup)}")
        } finally {
            RootShell.run("rm -rf ${q(stage)}")
        }
    }

    private fun root(command: String, input: ByteArray? = null): String {
        val result = if (input == null) RootShell.run(command) else RootShell.run(command, input)
        if (result.exitCode != 0) throw IOException("Native plugin operation failed: ${result.output.trim()}")
        return result.output.trim()
    }

    companion object {
        private val lock = Any()
        private const val SLOTS = 16
        private const val ALLOWED = "debug.dejavu.plugins.allowed"
        private const val HASH = "debug.dejavu.plugins.sha256"
        private const val SKILL_HASH = "debug.dejavu.plugins.skill.sha256"
        private const val TRUST_LOCK = "/data/adb/dejavu/.plugin-trust.lock"
        private const val LOCK_READY = "DEJAVU_PLUGIN_TRUST_LOCK_READY"
        private fun q(value: String) = RootShell.quote(value)
        private fun validateName(name: String) {
            require(name.length in 1..120 && name.matches(Regex("[A-Za-z0-9_][A-Za-z0-9_.-]*"))) { "Unsafe plugin filename: $name" }
        }
        private fun artifact(path: String): NativePluginArtifact {
            require(path.startsWith("$DEFAULT_PLUGIN_DIRECTORY/")) { "Plugin is outside the default library" }
            val relative = path.removePrefix("$DEFAULT_PLUGIN_DIRECTORY/")
            val parts = relative.split('/')
            require(parts.size in 1..32 && relative.length <= 400) { "Invalid native plugin resource path" }
            parts.forEach(::validateName)
            require(path.endsWith(".so") || path.endsWith(".c")) { "Expected a .so library or JIT .c source" }
            val filename = parts.last()
            return NativePluginArtifact(path, filename.removeSuffix(".so").removeSuffix(".c").removePrefix("libdejavu_"), path.endsWith(".c"))
        }
        private fun limit(path: String): Int = if (path.endsWith(".c")) 512 * 1024 else 32 * 1024 * 1024
        private fun validateBytes(name: String, bytes: ByteArray) {
            require(bytes.isNotEmpty()) { "Plugin is empty: $name" }
            if (name.endsWith(".c")) {
                require(decodePluginUtf8(bytes).contains("dejavu_jit_plugin_init")) { "Not a Dejavu JIT plugin: $name" }
            } else {
                require(bytes.size >= 4 && bytes.take(4) == listOf(0x7f.toByte(), 0x45.toByte(), 0x4c.toByte(), 0x46.toByte())) {
                    "Not an ELF plugin: $name"
                }
            }
        }
        private fun targetDirectories(process: String): List<String> {
            require(process.matches(Regex("[A-Za-z0-9_.]+(?::[A-Za-z0-9_.-]+)?"))) { "Invalid target process" }
            val pkg = process.substringBefore(':')
            return listOf("/data/user_de/0/$pkg/files", "/data/user/0/$pkg/files", "/data/data/$pkg/files")
        }
        private fun directoryGuard(path: String): String = path.split('/').filter { it.isNotEmpty() }.let { parts ->
            parts.indices.joinToString(" && ") { index ->
                val directory = "/" + parts.take(index + 1).joinToString("/")
                "[ -d ${q(directory)} ] && [ ! -L ${q(directory)} ]"
            }
        }
    }
}
