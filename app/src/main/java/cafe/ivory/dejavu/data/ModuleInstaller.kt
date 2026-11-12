package cafe.ivory.dejavu.data

import org.json.JSONObject
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.security.MessageDigest
import java.util.UUID
import java.util.concurrent.atomic.AtomicBoolean
import java.util.zip.ZipInputStream

internal enum class RootProvider { APATCH, KERNELSU, MAGISK }

internal data class ModuleBundle(
    val id: String,
    val version: String,
    val versionCode: Int,
    val sha256: String,
    val archive: ByteArray,
)

/** Only the verified module shipped inside the APK can reach the root installer. */
internal object ModulePayload {
    const val MODULE_ID = "dejavu_zygisk"
    const val MAX_ARCHIVE_BYTES = 64 * 1024 * 1024
    private const val MAX_EXPANDED_BYTES = 256L * 1024 * 1024
    private val versionPattern = Regex("v[0-9]+(?:\\.[0-9]+){1,3}(?:[-+][0-9A-Za-z.-]+)?")

    fun read(archive: ByteArray, metadataJson: String): ModuleBundle {
        require(archive.isNotEmpty() && archive.size <= MAX_ARCHIVE_BYTES) { "Bundled module archive size is invalid" }
        require(metadataJson.length <= 4096) { "Bundled module metadata is too large" }
        val metadata = JSONObject(metadataJson)
        val id = metadata.getString("id")
        val version = metadata.getString("version")
        val code = positiveInt(metadata.get("versionCode"), "versionCode")
        val size = positiveInt(metadata.get("size"), "size")
        val hash = metadata.getString("sha256")
        require(id == MODULE_ID) { "Bundled module identifier is invalid" }
        require(version.length <= 64 && versionPattern.matches(version)) { "Bundled module version is invalid" }
        require(size == archive.size) { "Bundled module size does not match metadata" }
        require(hash.matches(Regex("[0-9a-f]{64}"))) { "Bundled module checksum is invalid" }
        val owned = archive.copyOf()
        require(checksum(owned) == hash) { "Bundled module checksum does not match metadata" }
        val properties = archiveProperties(owned)
        require(properties["id"] == id && properties["version"] == version &&
            properties["versionCode"] == code.toString()) { "Bundled module identity or version does not match metadata" }
        return ModuleBundle(id, version, code, hash, owned)
    }

    internal fun validate(bundle: ModuleBundle): ModuleBundle = read(bundle.archive, JSONObject()
        .put("id", bundle.id).put("version", bundle.version).put("versionCode", bundle.versionCode)
        .put("sha256", bundle.sha256).put("size", bundle.archive.size).toString())

    internal fun checksum(bytes: ByteArray): String = MessageDigest.getInstance("SHA-256")
        .digest(bytes).joinToString("") { "%02x".format(it.toInt() and 255) }

    private fun positiveInt(value: Any, field: String): Int {
        require(value is Int || value is Long) { "Bundled module $field must be an integer" }
        val number = (value as Number).toLong()
        require(number in 1..Int.MAX_VALUE.toLong()) { "Bundled module $field is invalid" }
        return number.toInt()
    }

    private fun archiveProperties(archive: ByteArray): Map<String, String> {
        var prop: ByteArray? = null
        var expanded = 0L
        val names = hashSetOf<String>()
        ZipInputStream(ByteArrayInputStream(archive)).use { zip ->
            val buffer = ByteArray(8192)
            while (true) {
                val entry = zip.nextEntry ?: break
                val name = entry.name
                val canonicalName = name.removeSuffix("/")
                val parts = canonicalName.split('/')
                require(name.length <= 1024 && !name.startsWith('/') && '\\' !in name && ':' !in name &&
                    name.none { it.code < 32 || it.code == 127 } &&
                    parts.none { it.isEmpty() || it == "." || it == ".." }) { "Bundled module has an unsafe archive path" }
                require(names.add(canonicalName) && names.size <= 4096) { "Bundled module has duplicate or too many archive entries" }
                val output = if (name == "module.prop" && !entry.isDirectory) ByteArrayOutputStream() else null
                while (true) {
                    val count = zip.read(buffer)
                    if (count < 0) break
                    expanded += count
                    require(expanded <= MAX_EXPANDED_BYTES) { "Bundled module expands beyond its size limit" }
                    if (output != null) {
                        require(output.size() + count <= 64 * 1024) { "Bundled module properties are too large" }
                        output.write(buffer, 0, count)
                    }
                }
                if (output != null) prop = output.toByteArray()
                zip.closeEntry()
            }
        }
        val text = requireNotNull(prop) { "Bundled module properties are missing" }.toString(Charsets.UTF_8)
        val values = linkedMapOf<String, String>()
        text.lineSequence().map(String::trim).filter { it.isNotEmpty() && !it.startsWith('#') }.forEach { line ->
            val separator = line.indexOf('=')
            require(separator > 0) { "Bundled module properties are invalid" }
            val key = line.substring(0, separator).trim()
            require(key !in values) { "Bundled module properties are duplicated" }
            values[key] = line.substring(separator + 1).trim()
        }
        return values
    }
}

internal data class ModuleInstallSnapshot(
    val rootGranted: Boolean = false,
    val provider: RootProvider? = null,
    val installedVersion: String? = null,
    val stagedVersion: String? = null,
    val disabled: Boolean = false,
    val removalPending: Boolean = false,
    val rebootPending: Boolean = false,
    val zygiskPresent: Boolean = false,
    val installationRunning: Boolean = false,
)

/** Uses the authorized su entry, then its official module installer. Never reboots. */
internal class ModuleInstaller(
    private val shell: (String, Long) -> ShellResult = { command, timeout -> RootShell.run(command, timeout) },
    private val transfer: (String, ByteArray, Long) -> ShellResult = { command, bytes, timeout -> RootShell.run(command, bytes, timeout) },
    private val suExecutable: String = RootShell.commandArguments("").first(),
) {
    companion object {
        const val MAX_LOG_CHARS = 256 * 1024
        const val INSTALL_TIMEOUT_SECONDS = 180L
        private const val MODULE = "/data/adb/modules/${ModulePayload.MODULE_ID}"
        private const val STAGED = "/data/adb/modules_update/${ModulePayload.MODULE_ID}"
        private const val INSTALL_LOCK = "/data/adb/.dejavu-install.lock"
    }

    private val installing = AtomicBoolean()

    fun inspect(): ModuleInstallSnapshot {
        val result = shell(inspectionScript(), 12)
        if (result.exitCode != 0) throw IOException(
            "Unable to inspect module installation (exit ${result.exitCode}): ${result.output.trim().take(1024)}",
        )
        val fields = linkedMapOf<String, String>()
        result.output.lineSequence().forEach { line ->
            val split = line.indexOf('=')
            if (split > 0) {
                val key = line.substring(0, split)
                if (fields.put(key, line.substring(split + 1)) != null) {
                    throw IOException("Duplicate module inspection field: ${key.take(64)}")
                }
            }
        }
        when (fields["root"]) {
            "0" -> return ModuleInstallSnapshot()
            "1" -> Unit
            else -> throw IOException("Module inspection returned an invalid root status")
        }
        val providerName = fields["provider"] ?: throw IOException("Module inspection omitted its root provider status")
        val provider = RootProvider.entries.firstOrNull { it.name == providerName }
        if (providerName.isNotEmpty() && provider == null) throw IOException("Module inspection returned an invalid root provider status")
        for (key in listOf("disabled", "remove", "update", "pending", "zygisk", "installing")) {
            fields[key]?.let { if (it != "0" && it != "1") throw IOException("Invalid module inspection flag: $key") }
        }
        fun version(key: String): String? = fields[key]?.takeIf {
            it.length in 1..64 && it.none { char -> char.code < 32 || char.code == 127 }
        }
        val staged = version("staged")
        return ModuleInstallSnapshot(
            rootGranted = true,
            provider = provider,
            installedVersion = version("installed"),
            stagedVersion = staged,
            disabled = fields["disabled"] == "1",
            removalPending = fields["remove"] == "1",
            rebootPending = fields["update"] == "1" || fields["remove"] == "1" ||
                fields["pending"] == "1" || staged != null,
            zygiskPresent = fields["zygisk"] == "1",
            installationRunning = fields["installing"] == "1",
        )
    }

    fun install(bundle: ModuleBundle, sdk: Int, abis: List<String>): ShellResult {
        if (!installing.compareAndSet(false, true)) return ShellResult(75, "A module installation is already running")
        try {
            if (sdk !in 26..35 || "arm64-v8a" !in abis) {
                return ShellResult(1, "The bundled module requires arm64 and Android API 26–35")
            }
            val verified = try { ModulePayload.validate(bundle) } catch (error: Exception) {
                return ShellResult(1, "Invalid bundled module: ${error.message.orEmpty().take(512)}")
            }
            val status = inspect()
            if (!status.rootGranted) return ShellResult(1, "Root authorization is required to install Dejavu")
            if (status.installationRunning) return ShellResult(75, "A Dejavu module installation is already running")
            if (status.rebootPending || status.removalPending) return ShellResult(75,
                "A Dejavu module change is waiting for reboot; finish or cancel it before installing again")
            val provider = status.provider ?: return ShellResult(1, "The authorized su entry is not a supported root provider")
            val marker = "DEJAVU_INSTALL_EXIT_" + UUID.randomUUID().toString().replace("-", "")
            val result = transfer(installationScript(provider, verified, marker), verified.archive, INSTALL_TIMEOUT_SECONDS)
            if (result.exitCode != 0) return result.copy(output = result.output.take(MAX_LOG_CHARS))
            val terminal = "\n$marker="
            val offset = result.output.lastIndexOf(terminal)
            if (offset < 0) return ShellResult(1, "Module installer did not report its completion status")
            val exitCode = result.output.substring(offset + terminal.length).trim().toIntOrNull()
                ?.takeIf { it in 0..255 } ?: return ShellResult(1, "Module installer returned an invalid completion status")
            val log = result.output.substring(0, offset).take(MAX_LOG_CHARS).trimEnd()
            return ShellResult(exitCode, if (exitCode == 0) {
                val notice = "Module installation completed. A reboot is required to load the new framework."
                "${log.take(MAX_LOG_CHARS - notice.length - 1)}\n$notice".trimStart()
            } else log.ifBlank { "Module installation failed (exit $exitCode)" })
        } catch (error: Exception) {
            return ShellResult(1, "Module installation failed: ${error.message.orEmpty().take(512)}")
        } finally {
            installing.set(false)
        }
    }

    private fun identityScript(): String = """
        dejavu_identity=${'$'}(${RootShell.quote(suExecutable)} -v 2>/dev/null) || dejavu_identity=
        case "${'$'}dejavu_identity" in
            *:APatch|*':APatch(FolkPatch)') dejavu_provider=APATCH ;;
            *:KernelSU) dejavu_provider=KERNELSU ;;
            *:MAGISKSU) dejavu_provider=MAGISK ;;
            *) dejavu_provider= ;;
        esac
    """.trimIndent()

    private fun inspectionScript(): String = """
        [ "${'$'}(id -u)" = 0 ] || { printf 'root=0\n'; exit 0; }
        printf 'root=1\n'
        ${identityScript()}
        printf 'provider=%s\n' "${'$'}dejavu_provider"
        command -v flock >/dev/null 2>&1 || { echo 'The module installation lock utility is unavailable'; exit 1; }
        [ ! -L '$INSTALL_LOCK' ] || { echo 'The module installation lock must not be a symbolic link'; exit 1; }
        umask 077
        exec 9>'$INSTALL_LOCK' || exit 1
        if flock -n 9 9>&9; then
            flock -u 9 9>&9 || exit 1
            printf 'installing=0\n'
        else
            printf 'installing=1\n'
        fi
        dejavu_version() {
            [ -f "${'$'}1/module.prop" ] && [ ! -L "${'$'}1" ] && [ ! -L "${'$'}1/module.prop" ] || return 0
            head -c 65536 "${'$'}1/module.prop" | sed -n 's/^version=//p' | head -n 1
        }
        printf 'installed=%s\n' "${'$'}(dejavu_version '$MODULE')"
        printf 'staged=%s\n' "${'$'}(dejavu_version '$STAGED')"
        dejavu_pending=0
        if [ -e '$STAGED' ] || [ -L '$STAGED' ]; then dejavu_pending=1; fi
        printf 'pending=%s\n' "${'$'}dejavu_pending"
        for dejavu_flag in disable remove update; do
            dejavu_value=0
            [ ! -e '$MODULE'/"${'$'}dejavu_flag" ] || dejavu_value=1
            case "${'$'}dejavu_flag" in disable) dejavu_key=disabled ;; *) dejavu_key=${'$'}dejavu_flag ;; esac
            printf '%s=%s\n' "${'$'}dejavu_key" "${'$'}dejavu_value"
        done
        dejavu_zygisk=0
        for dejavu_directory in /data/adb/modules/*; do
            [ -d "${'$'}dejavu_directory" ] && [ ! -L "${'$'}dejavu_directory" ] || continue
            [ ! -e "${'$'}dejavu_directory/disable" ] && [ ! -e "${'$'}dejavu_directory/remove" ] || continue
            case "${'$'}{dejavu_directory##*/}" in
                zygisk_lsposed) ;;
                zygisk*|rezygisk*) dejavu_zygisk=1 ;;
            esac
        done
        if [ "${'$'}dejavu_provider" = MAGISK ] && command -v magisk >/dev/null 2>&1; then
            dejavu_setting=${'$'}(magisk --sqlite 'SELECT value FROM settings WHERE key="zygisk"' 2>/dev/null)
            [ "${'$'}dejavu_setting" != 'value=1' ] || dejavu_zygisk=1
        fi
        printf 'zygisk=%s\n' "${'$'}dejavu_zygisk"
    """.trimIndent()

    private fun installationScript(provider: RootProvider, bundle: ModuleBundle, marker: String): String {
        val locate = when (provider) {
            RootProvider.APATCH -> """
                for dejavu_candidate in /data/adb/apd /data/adb/ap/bin/apd; do
                    if [ -x "${'$'}dejavu_candidate" ]; then dejavu_installer=${'$'}dejavu_candidate; break; fi
                done
            """.trimIndent()
            RootProvider.KERNELSU -> """
                for dejavu_candidate in /data/adb/ksud /data/adb/ksu/bin/ksud; do
                    if [ -x "${'$'}dejavu_candidate" ]; then dejavu_installer=${'$'}dejavu_candidate; break; fi
                done
            """.trimIndent()
            RootProvider.MAGISK -> """
                dejavu_installer=${'$'}(command -v magisk 2>/dev/null) || dejavu_installer=
                for dejavu_candidate in /debug_ramdisk/magisk /sbin/magisk /data/adb/magisk/magisk; do
                    [ -z "${'$'}dejavu_installer" ] || break
                    if [ -x "${'$'}dejavu_candidate" ]; then dejavu_installer=${'$'}dejavu_candidate; fi
                done
            """.trimIndent()
        }
        val arguments = if (provider == RootProvider.MAGISK) "--install-module" else "module install"
        return """
            set -u
            umask 077
            [ "${'$'}(id -u)" = 0 ] || { echo 'Root authorization is required'; exit 1; }
            ${identityScript()}
            [ "${'$'}dejavu_provider" = '${provider.name}' ] || { echo 'Root provider changed before installation'; exit 1; }
            dejavu_installer=
            $locate
            [ -n "${'$'}dejavu_installer" ] && [ -x "${'$'}dejavu_installer" ] || { echo 'The root provider module installer is unavailable'; exit 1; }
            for dejavu_command in mktemp sha256sum head cat timeout flock; do
                command -v "${'$'}dejavu_command" >/dev/null 2>&1 || { echo 'A required installation utility is unavailable'; exit 1; }
            done
            [ ! -L '$INSTALL_LOCK' ] || { echo 'The module installation lock must not be a symbolic link'; exit 1; }
            exec 9>'$INSTALL_LOCK' || exit 1
            flock -n 9 9>&9 || { echo 'A Dejavu module installation is already running'; exit 75; }
            if [ -e '$MODULE/remove' ] || [ -L '$MODULE/remove' ] ||
               [ -e '$MODULE/update' ] || [ -L '$MODULE/update' ] ||
               [ -e '$STAGED' ] || [ -L '$STAGED' ]; then
                echo 'A Dejavu module change is waiting for reboot; finish or cancel it before installing again'
                exit 75
            fi
            dejavu_stage=${'$'}(mktemp -d /data/adb/.dejavu-install.XXXXXX) || exit 1
            dejavu_cleanup() {
                dejavu_status=${'$'}?
                trap - EXIT HUP INT TERM
                if ! rm -rf -- "${'$'}dejavu_stage"; then
                    echo 'Unable to clean the private module staging directory' >&2
                    [ "${'$'}dejavu_status" -ne 0 ] || dejavu_status=1
                fi
                exit "${'$'}dejavu_status"
            }
            trap dejavu_cleanup EXIT
            trap 'exit 130' HUP INT TERM
            chmod 700 "${'$'}dejavu_stage" || exit 1
            dejavu_archive="${'$'}dejavu_stage/module.zip"
            cat > "${'$'}dejavu_archive" || exit 1
            [ "${'$'}(wc -c < "${'$'}dejavu_archive")" -eq ${bundle.archive.size} ] || { echo 'Transferred module size mismatch'; exit 1; }
            dejavu_checksum=${'$'}(sha256sum "${'$'}dejavu_archive") || exit 1
            dejavu_checksum=${'$'}{dejavu_checksum%% *}
            [ "${'$'}dejavu_checksum" = '${bundle.sha256}' ] || { echo 'Transferred module checksum mismatch'; exit 1; }
            head -c 1 </dev/null >/dev/null || { echo 'Byte-limited log collection is unavailable'; exit 1; }
            {
                timeout -k 5 150 "${'$'}dejavu_installer" $arguments "${'$'}dejavu_archive"
                dejavu_status=${'$'}?
                printf '%s\n' "${'$'}dejavu_status" > "${'$'}dejavu_stage/status"
            } 2>&1 | { head -c $MAX_LOG_CHARS; cat >/dev/null; }
            IFS= read -r dejavu_status < "${'$'}dejavu_stage/status" || exit 1
            case "${'$'}dejavu_status" in ''|*[!0-9]*) exit 1 ;; esac
            printf '\n%s=%s\n' '$marker' "${'$'}dejavu_status"
        """.trimIndent()
    }
}
