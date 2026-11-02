package cafe.ivory.dejavu.data

import org.json.JSONArray
import org.json.JSONObject
import java.security.MessageDigest

data class ProjectedPluginEvent(val item: LivePluginEvent, val dedupKey: String?)

/** Interpret only the display and identity fields explicitly declared by a plugin. */
fun projectPluginEvent(
    event: JSONObject,
    spec: PluginEventSpec,
    fallbackTitle: String,
): ProjectedPluginEvent {
    spec.validate()
    fun text(field: String): String? = event.opt(field)?.takeUnless { it === JSONObject.NULL }?.toString()
    fun identity(fields: List<String>): String? {
        if (fields.isEmpty()) return null
        val values = fields.map { field ->
            text(field)?.takeIf { it.isNotEmpty() && it !in spec.dedupIgnoreValues } ?: return null
        }
        val identity = JSONArray().put(JSONArray(fields)).put(JSONArray(values)).toString()
        return MessageDigest.getInstance("SHA-256").digest(identity.toByteArray(Charsets.UTF_8))
            .joinToString("") { "%02x".format(it.toInt() and 255) }
    }
    return ProjectedPluginEvent(
        item = LivePluginEvent(
            title = (text(spec.titleField) ?: fallbackTitle).take(1024),
            content = (text(spec.contentField) ?: event.toString()).take(16384),
            meta = text(spec.metaField).orEmpty().take(1024),
        ),
        dedupKey = identity(spec.dedupFields) ?: identity(spec.dedupFallbackFields),
    )
}
