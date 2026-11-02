package cafe.ivory.dejavu.data

import java.security.MessageDigest

/** A source in the private library, separate from any process's loaded copy. */
data class NativePluginArtifact(
    val path: String,
    val name: String,
    val jit: Boolean,
    val id: String? = null,
    val version: String = "",
    val description: String = "",
) {
    internal val deploymentName: String
        get() = MessageDigest.getInstance("SHA-256").digest(path.toByteArray(Charsets.UTF_8))
            .take(12).joinToString("") { "%02x".format(it.toInt() and 255) } +
            if (jit) ".c" else ".so"
}
