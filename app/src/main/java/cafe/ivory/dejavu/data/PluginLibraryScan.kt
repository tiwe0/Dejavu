package cafe.ivory.dejavu.data

import java.io.IOException
import java.util.Base64

private const val SCAN_ERROR_PREFIX = "DEJAVU_SCAN_ERROR:"

internal data class StoredPluginManifest(val manifest: PluginPackageManifest, val byteSize: Int)

/** One manifest snapshot can be shared by the Lua and native library readers. */
internal data class PluginLibraryScan(
    val manifests: List<StoredPluginManifest>,
    val errors: List<String>,
) {
    companion object {
        fun read(shell: ShellExecutor = RootShell): PluginLibraryScan {
            val result = shell.run("""
                if [ ! -e ${shell.quote(DEFAULT_PLUGIN_DIRECTORY)} ] && [ ! -L ${shell.quote(DEFAULT_PLUGIN_DIRECTORY)} ]; then exit 0; fi
                ${pluginDirectoryGuard(DEFAULT_PLUGIN_DIRECTORY, shell::quote)} || exit 1
                for directory in ${shell.quote(DEFAULT_PLUGIN_DIRECTORY)}/*; do
                    [ -d "${'$'}directory" ] && [ ! -L "${'$'}directory" ] || continue
                    path="${'$'}directory/$PLUGIN_MANIFEST_FILE"
                    [ -e "${'$'}path" ] || [ -L "${'$'}path" ] || continue
                    printf '%s\000' "${'$'}{directory##*/}" || exit 1
                    if (
                        [ ! -L "${'$'}path" ] && [ -f "${'$'}path" ] || exit 1
                        size=${'$'}(wc -c < "${'$'}path") || exit 1
                        [ "${'$'}size" -le $MAX_PLUGIN_MANIFEST_BYTES ] || exit 1
                        base64 "${'$'}path"
                    ) 2>/dev/null; then
                        printf '\000ok\000' || exit 1
                    else
                        printf '\n${SCAN_ERROR_PREFIX}manifest.json is linked, oversized or unreadable\000error\000' || exit 1
                    fi
                done
                exit 0
            """.trimIndent())
            if (result.exitCode != 0) return PluginLibraryScan(emptyList(), listOf(
                "Unable to scan $DEFAULT_PLUGIN_DIRECTORY: ${result.output.trim()}",
            ))
            val errors = mutableListOf<String>()
            val manifests = try {
                pluginScanResults(result.output).mapNotNull { (id, encoded) ->
                    try {
                        validatePluginId(id)
                        val bytes = decodePluginScanBytes(encoded.getOrThrow())
                        require(bytes.size <= MAX_PLUGIN_MANIFEST_BYTES) { "Plugin manifest is too large" }
                        val manifest = PluginPackageManifest.parse(decodePluginUtf8(bytes))
                        require(manifest.id == id) { "Plugin identifier does not match its directory" }
                        StoredPluginManifest(manifest, bytes.size)
                    } catch (error: Exception) {
                        errors += "$id: ${error.message ?: "invalid manifest"}"
                        null
                    }
                }
            } catch (error: Exception) {
                errors += "Unable to read plugin manifest scan: ${error.message}"
                emptyList()
            }
            return PluginLibraryScan(manifests, errors)
        }
    }
}

/** Execute independent package checks in one root shell, retaining per-package errors. */
internal fun runPluginScanChecks(
    checks: Map<String, String>,
    shell: ShellExecutor = RootShell,
): Map<String, Result<String>> {
    if (checks.isEmpty()) return emptyMap()
    val batches = mutableListOf<String>()
    val batch = StringBuilder()
    var batchBytes = 0
    checks.forEach { (id, script) ->
        // Write the status after the data so base64 can stream directly to stdout.
        val command = """
            printf '%s\000' ${shell.quote(id)} || exit 1
            if (
                dejavu_plugin_root=${shell.quote("$DEFAULT_PLUGIN_DIRECTORY/$id/")}
                $script
            ) 2>/dev/null; then
                printf '\000ok\000' || exit 1
            else
                printf '\000error\000' || exit 1
            fi
        """.trimIndent()
        val bytes = command.toByteArray(Charsets.UTF_8).size + 1
        // Keep su's command argument below Android's per-argument limit for large libraries.
        if (batch.isNotEmpty() && batchBytes + bytes > 64 * 1024) {
            batches += batch.toString()
            batch.clear()
            batchBytes = 0
        }
        batch.appendLine(command)
        batchBytes += bytes
    }
    if (batch.isNotEmpty()) batches += batch.toString()
    val fileCheck = """
        scan_plugin_error() {
            dejavu_failed_resource=${'$'}{1#"${'$'}dejavu_plugin_root"}
            printf '\n${SCAN_ERROR_PREFIX}%s (%s)\n' "${'$'}dejavu_failed_resource" "${'$'}2"
            return 1
        }
        scan_plugin_file() {
            dejavu_guard_path=${'$'}{1%/*}
            while [ -n "${'$'}dejavu_guard_path" ]; do
                [ -d "${'$'}dejavu_guard_path" ] && [ ! -L "${'$'}dejavu_guard_path" ] || {
                    scan_plugin_error "${'$'}1" 'missing or linked parent directory'
                    return 1
                }
                dejavu_guard_path=${'$'}{dejavu_guard_path%/*}
            done
            [ ! -L "${'$'}1" ] || { scan_plugin_error "${'$'}1" 'symbolic link'; return 1; }
            [ -f "${'$'}1" ] || { scan_plugin_error "${'$'}1" 'missing or non-regular file'; return 1; }
            dejavu_size=${'$'}(wc -c < "${'$'}1") || { scan_plugin_error "${'$'}1" 'cannot read size'; return 1; }
            [ "${'$'}dejavu_size" -le "${'$'}2" ] || { scan_plugin_error "${'$'}1" 'exceeds allowed size'; return 1; }
        }
    """.trimIndent()
    val results = linkedMapOf<String, Result<String>>()
    for (command in batches) {
        val result = shell.run("$fileCheck\n$command")
        if (result.exitCode != 0) throw IOException("Unable to validate plugin resources (root command exit ${result.exitCode})")
        results.putAll(pluginScanResults(result.output))
    }
    require(results.keys == checks.keys) { "Incomplete plugin resource scan" }
    return results
}

/** Each stdout record is ID, payload, then status, separated and terminated by NUL. */
private fun pluginScanResults(output: String): Map<String, Result<String>> {
    if (output.isBlank()) return emptyMap()
    val fields = output.split('\u0000')
    require(fields.last().isBlank() && (fields.size - 1) % 3 == 0) { "Malformed plugin scan response" }
    val results = linkedMapOf<String, Result<String>>()
    for (index in 0 until fields.lastIndex step 3) {
        val id = fields[index]
        require(id !in results) { "Duplicate plugin scan result" }
        val payload = fields[index + 1]
        results[id] = when (fields[index + 2]) {
            "ok" -> Result.success(payload)
            "error" -> {
                val lastLine = payload.trimEnd('\r', '\n').substringAfterLast('\n')
                val diagnostic = if (lastLine.startsWith(SCAN_ERROR_PREFIX)) {
                    lastLine.removePrefix(SCAN_ERROR_PREFIX)
                } else ""
                Result.failure(IOException(diagnostic.trim().take(1024).ifBlank {
                    "Plugin files are invalid or unreadable"
                }))
            }
            else -> throw IOException("Invalid plugin scan status")
        }
    }
    return results
}

internal fun decodePluginScanBytes(encoded: String): ByteArray =
    Base64.getDecoder().decode(encoded.filterNot { it == '\r' || it == '\n' })
