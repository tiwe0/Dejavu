package cafe.ivory.dejavu.data

import org.json.JSONArray
import org.json.JSONObject
import java.nio.ByteBuffer
import java.security.MessageDigest
import java.util.Base64

data class PluginScopesSnapshot(val scopes: Map<String, List<String>>, val targets: List<String>)

/** Scope ownership and the native target union share one atomic configuration file. */
class PluginScopeStore(private val shell: ShellExecutor = RootShell) {
    fun read(): PluginScopesSnapshot = load().snapshot()

    fun setScope(id: String, targets: List<String>): PluginScopesSnapshot {
        require(id.matches(Regex("[a-z][a-z0-9_-]{0,63}"))) { "Invalid plugin ID" }
        val normalized = normalize(targets)
        return update { old ->
            old.copy(scopes = old.scopes.toMutableMap().apply {
                if (normalized.isEmpty()) remove(id) else put(id, normalized)
            })
        }
    }

    fun removeManualTarget(process: String): PluginScopesSnapshot {
        normalize(listOf(process))
        return update { it.copy(manual = it.manual - process) }
    }

    private data class State(val manual: List<String>, val scopes: Map<String, List<String>>, val bytes: ByteArray) {
        fun snapshot() = PluginScopesSnapshot(scopes.toSortedMap(), normalize(manual + scopes.values.flatten()))
        fun encode(): ByteArray {
            val scopeJson = JSONObject()
            scopes.toSortedMap().forEach { (id, targets) -> scopeJson.put(id, JSONArray(targets)) }
            val metadata = JSONObject().put("manual", JSONArray(manual)).put("scopes", scopeJson)
            return (PREFIX + metadata.toString() + "\n" + snapshot().targets.joinToString("") { "$it\n" }).toByteArray(Charsets.UTF_8).also {
                require(it.size <= MAX_BYTES) { "Plugin scopes exceed the 16 KiB target configuration limit" }
            }
        }
    }

    private fun load(): State {
        val guard = pluginDirectoryGuard(DIRECTORY, shell::quote)
        val result = shell.run("set -e\n$guard || exit 1\n[ -f '$PATH' ] && [ ! -L '$PATH' ] || exit 1\n[ \"\$(wc -c < '$PATH')\" -le $MAX_BYTES ]\nbase64 < '$PATH'")
        check(result.exitCode == 0) { "Cannot read plugin scopes: ${result.output.trim()}" }
        val bytes = Base64.getMimeDecoder().decode(result.output.trim())
        require(bytes.size <= MAX_BYTES) { "Target configuration exceeds 16 KiB" }
        val text = Charsets.UTF_8.newDecoder().decode(ByteBuffer.wrap(bytes)).toString()
        val lines = text.lineSequence().map(String::trim).toList()
        val effective = normalize(lines.filter { it.isNotEmpty() && !it.startsWith('#') })
        val headers = lines.filter { it.startsWith("# dejavu-plugin-scopes-") }
        if (headers.isEmpty()) return State(effective, emptyMap(), bytes)
        require(headers.size == 1 && headers.single().startsWith(PREFIX)) { "Invalid plugin scope metadata version" }
        val json = JSONObject(headers.single().removePrefix(PREFIX))
        require(json.keys().asSequence().toSet() == setOf("manual", "scopes")) { "Invalid plugin scope metadata" }
        val manual = array(json.getJSONArray("manual"))
        val scopesJson = json.getJSONObject("scopes")
        val scopes = scopesJson.keys().asSequence().toSet().associateWith { id ->
            require(id.matches(Regex("[a-z][a-z0-9_-]{0,63}"))) { "Invalid plugin ID in scope metadata" }
            array(scopesJson.getJSONArray(id))
        }.toSortedMap()
        val state = State(manual, scopes, bytes)
        require(state.snapshot().targets == effective) { "Plugin scope metadata does not match target processes" }
        return state
    }

    private fun update(transform: (State) -> State): PluginScopesSnapshot {
        val guard = pluginDirectoryGuard(DIRECTORY, shell::quote)
        repeat(4) {
            val previous = load()
            val updated = transform(previous)
            val bytes = updated.encode()
            val expected = MessageDigest.getInstance("SHA-256").digest(previous.bytes).joinToString("") { "%02x".format(it) }
            val command = """
                set -e
                umask 022
                $guard || exit 1
                chmod 755 '$DIRECTORY'
                [ ! -L '$LOCK' ] && { [ ! -e '$LOCK' ] || [ -f '$LOCK' ]; } || exit 1
                exec 9>'$LOCK'
                chmod 600 '$LOCK'
                /system/bin/flock -n 9 9>&9 || exit 73
                [ -f '$PATH' ] && [ ! -L '$PATH' ] || exit 1
                actual=${'$'}(sha256sum '$PATH'); actual=${'$'}{actual%% *}
                [ "${'$'}actual" = '$expected' ] || exit 73
                stage=${'$'}(mktemp '$DIRECTORY/.targets.XXXXXX')
                trap 'rm -f "${'$'}stage"' EXIT
                cat > "${'$'}stage"
                [ "${'$'}(wc -c < "${'$'}stage")" -eq ${bytes.size} ]
                chmod 644 "${'$'}stage"
                chown 0:0 "${'$'}stage"
                if command -v chcon >/dev/null 2>&1; then
                    chcon u:object_r:system_file:s0 '$DIRECTORY' 2>/dev/null || true
                    chcon u:object_r:system_file:s0 "${'$'}stage" 2>/dev/null || true
                fi
                mv -f "${'$'}stage" '$PATH'
            """.trimIndent()
            val result = shell.run(command, bytes)
            if (result.exitCode == 0) return updated.snapshot()
            check(result.exitCode == 73) { "Cannot save plugin scopes: ${result.output.trim()}" }
            Thread.sleep(25L * (it + 1))
        }
        error("Plugin scopes changed concurrently; retry the operation")
    }

    companion object {
        private const val DIRECTORY = "$MODULE_DIR/config"
        private const val PATH = "$DIRECTORY/targets.txt"
        private const val LOCK = "$DIRECTORY/.targets.lock"
        private const val PREFIX = "# dejavu-plugin-scopes-v1 "
        private const val MAX_BYTES = 16 * 1024
        private fun normalize(targets: List<String>): List<String> = targets.onEach {
            require(it.matches(Regex("[A-Za-z0-9_.:-]{1,128}"))) { "Invalid target process: $it" }
        }.distinct().sorted()
        private fun array(array: JSONArray): List<String> = normalize((0 until array.length()).map {
            require(array.get(it) is String) { "Invalid process in scope metadata" }
            array.getString(it)
        })
    }
}
