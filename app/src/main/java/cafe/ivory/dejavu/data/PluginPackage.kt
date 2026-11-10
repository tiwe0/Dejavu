package cafe.ivory.dejavu.data

import org.json.JSONArray
import org.json.JSONObject
import java.nio.ByteBuffer
import java.nio.CharBuffer
import java.nio.charset.CodingErrorAction

const val PLUGIN_MANIFEST_FILE = "manifest.json"
const val MAX_PLUGIN_PACKAGE_BYTES = 256 * 1024
const val MAX_PLUGIN_RPC_BYTES = 1024 * 1024
const val MAX_PLUGIN_FILES = 32
const val MAX_PLUGIN_MANIFEST_BYTES = 16 * 1024
private const val LOGCAT_EVENT_FORMAT = "urlencoded-json-v1"
private const val SOCKET_EVENT_FORMAT = "hook-socket-json-v1"

internal fun validatePluginId(id: String) {
    require(id.matches(Regex("[a-z][a-z0-9_-]{0,63}"))) { "Invalid plugin identifier: $id" }
}

internal fun validatePluginPath(path: String) {
    require(path.length in 1..240 && '\\' !in path &&
        !Regex("^[A-Za-z]:").containsMatchIn(path) && path.none { it.code < 32 || it.code == 127 }) {
        "Invalid plugin resource path: $path"
    }
    require(path.split('/').none { it.isEmpty() || it == "." || it == ".." }) {
        "Plugin resource paths must be normalized and relative: $path"
    }
    require(path != PLUGIN_MANIFEST_FILE) { "The manifest cannot be a code resource" }
}

fun decodePluginUtf8(bytes: ByteArray): String = Charsets.UTF_8.newDecoder()
    .onMalformedInput(CodingErrorAction.REPORT)
    .onUnmappableCharacter(CodingErrorAction.REPORT)
    .decode(ByteBuffer.wrap(bytes)).toString()

internal fun pluginUtf8Bytes(text: String): ByteArray {
    val buffer = Charsets.UTF_8.newEncoder()
        .onMalformedInput(CodingErrorAction.REPORT)
        .onUnmappableCharacter(CodingErrorAction.REPORT)
        .encode(CharBuffer.wrap(text))
    return ByteArray(buffer.remaining()).also { buffer.get(it) }
}

data class PluginEventSpec(
    val tag: String = "",
    val marker: String = "",
    val format: String = LOGCAT_EVENT_FORMAT,
    val titleField: String = "title",
    val contentField: String = "content",
    val metaField: String = "meta",
    val dedupFields: List<String> = emptyList(),
    val dedupFallbackFields: List<String> = emptyList(),
    val dedupIgnoreValues: List<String> = listOf(""),
    val topic: String? = null,
) {
    val usesSocket: Boolean get() = format == SOCKET_EVENT_FORMAT

    fun validate() {
        when (format) {
            LOGCAT_EVENT_FORMAT -> {
                require(tag.matches(Regex("[A-Za-z0-9_.-]{1,64}"))) { "Invalid plugin event tag" }
                require(marker.matches(Regex("[A-Za-z0-9_.:-]{1,64}"))) { "Invalid plugin event marker" }
                require(topic == null) { "Logcat events do not use a socket topic" }
            }
            SOCKET_EVENT_FORMAT -> {
                require(tag.isEmpty() && marker.isEmpty()) { "Socket events do not use a logcat tag or marker" }
                require(topic?.matches(Regex("[A-Za-z0-9_.-]{1,64}")) == true) { "Invalid plugin event topic" }
            }
            else -> throw IllegalArgumentException("Unsupported plugin event format: $format")
        }
        fun field(name: String) {
            require(name.matches(Regex("[A-Za-z_][A-Za-z0-9_.-]{0,63}"))) {
                "Invalid plugin event field: $name"
            }
        }
        listOf(titleField, contentField, metaField).forEach(::field)
        for (fields in listOf(dedupFields, dedupFallbackFields)) {
            require(fields.size <= 8 && fields.distinct().size == fields.size) {
                "Declare at most 8 distinct plugin event dedup fields"
            }
            fields.forEach(::field)
        }
        require(dedupIgnoreValues.size <= 8 && dedupIgnoreValues.distinct().size == dedupIgnoreValues.size &&
            dedupIgnoreValues.all { it.length <= 128 }) {
            "Declare at most 8 distinct plugin event ignored values of at most 128 characters"
        }
    }
}

data class PluginPackageManifest(
    val id: String,
    val name: String,
    val version: String,
    val description: String = "",
    val entry: String,
    val files: List<String>,
    val events: PluginEventSpec? = null,
    val schemaVersion: Int = 1,
    val kind: String = "lua",
    val ui: String? = null,
) {
    fun validate() {
        require(schemaVersion == 1) { "Unsupported plugin manifest version: $schemaVersion" }
        require(kind in setOf("lua", "native", "jit")) { "Unsupported plugin kind: $kind" }
        validatePluginId(id)
        require(name.isNotBlank() && name.length <= 120) { "Plugin name is missing or too long" }
        require(version.isNotBlank() && version.length <= 64) { "Plugin version is missing or too long" }
        require(description.length <= 2048) { "Plugin description is too long" }
        require(files.size in 1..MAX_PLUGIN_FILES) { "Declare between 1 and $MAX_PLUGIN_FILES resources" }
        require(files.distinct().size == files.size) { "Plugin resources contain duplicate paths" }
        files.forEach(::validatePluginPath)
        require(entry in files) { "Plugin entry must be declared in files" }
        require(when (kind) {
            "native" -> entry.endsWith(".so")
            "jit" -> entry.endsWith(".c")
            else -> true
        }) { "Plugin entry does not match kind: $kind" }
        require(kind == "lua" || events == null) { "Live events require a Lua package" }
        ui?.let { path ->
            require(kind == "lua") { "Plugin pages require a Lua package" }
            require(path.endsWith(".json") && path in files && path != entry) {
                "Plugin ui must be a declared JSON resource distinct from the entry"
            }
        }
        // A file cannot also be the parent directory of another declared file.
        require(files.none { file -> files.any { other -> other.startsWith("$file/") } }) {
            "Plugin resource paths conflict as files and directories"
        }
        events?.validate()
    }

    fun toJson(): String {
        validate()
        return JSONObject().apply {
            put("schemaVersion", schemaVersion)
            put("kind", kind)
            put("id", id)
            put("name", name)
            put("version", version)
            put("description", description)
            put("entry", entry)
            put("files", JSONArray(files))
            ui?.let { put("ui", it) }
            events?.let { spec ->
                put("events", JSONObject().apply {
                    put("format", spec.format)
                    if (spec.usesSocket) {
                        put("topic", spec.topic)
                    } else {
                        put("tag", spec.tag)
                        put("marker", spec.marker)
                    }
                    put("titleField", spec.titleField)
                    put("contentField", spec.contentField)
                    put("metaField", spec.metaField)
                    put("dedupFields", JSONArray(spec.dedupFields))
                    put("dedupFallbackFields", JSONArray(spec.dedupFallbackFields))
                    put("dedupIgnoreValues", JSONArray(spec.dedupIgnoreValues))
                })
            }
        }.toString()
    }

    companion object {
        fun parse(text: String): PluginPackageManifest {
            require(pluginUtf8Bytes(text).size <= MAX_PLUGIN_MANIFEST_BYTES) { "Plugin manifest is too large" }
            val json = JSONObject(text)
            fun string(key: String): String = (json.get(key) as? String)
                ?: throw IllegalArgumentException("Manifest $key must be a string")
            val schema = json.get("schemaVersion")
            require((schema is Int || schema is Long) && (schema as Number).toLong() == 1L) {
                "Unsupported plugin manifest version"
            }
            val files = json.getJSONArray("files")
            require(files.length() in 1..MAX_PLUGIN_FILES) { "Too many or missing plugin resources" }
            val events = if (json.has("events")) {
                val value = json.getJSONObject("events")
                fun eventString(key: String): String = (value.get(key) as? String)
                    ?: throw IllegalArgumentException("Manifest events.$key must be a string")
                fun eventStrings(key: String, default: List<String>): List<String> {
                    if (!value.has(key)) return default
                    val array = value.getJSONArray(key)
                    require(array.length() <= 8) { "Manifest events.$key has too many values" }
                    return (0 until array.length()).map { index ->
                        array.get(index) as? String
                            ?: throw IllegalArgumentException("Manifest events.$key must contain strings")
                    }
                }
                PluginEventSpec(
                    tag = if (value.has("tag")) eventString("tag") else "",
                    marker = if (value.has("marker")) eventString("marker") else "",
                    format = if (value.has("format")) eventString("format") else LOGCAT_EVENT_FORMAT,
                    titleField = if (value.has("titleField")) eventString("titleField") else "title",
                    contentField = if (value.has("contentField")) eventString("contentField") else "content",
                    metaField = if (value.has("metaField")) eventString("metaField") else "meta",
                    dedupFields = eventStrings("dedupFields", emptyList()),
                    dedupFallbackFields = eventStrings("dedupFallbackFields", emptyList()),
                    dedupIgnoreValues = eventStrings("dedupIgnoreValues", listOf("")),
                    topic = if (value.has("topic")) eventString("topic") else null,
                )
            } else null
            return PluginPackageManifest(
                id = string("id"), name = string("name"), version = string("version"),
                description = if (json.has("description")) string("description") else "",
                entry = string("entry"),
                files = (0 until files.length()).map { index ->
                    files.get(index) as? String
                        ?: throw IllegalArgumentException("Manifest files must contain strings")
                },
                events = events, schemaVersion = 1, kind = string("kind"),
                ui = if (json.has("ui")) string("ui") else null,
            ).also { it.validate() }
        }
    }
}

data class ImportedPluginPackage(
    val manifest: PluginPackageManifest,
    val sources: Map<String, String>,
) {
    val page: PluginPage? by lazy {
        manifest.ui?.let { path -> PluginPage.parse(sources.getValue(path), manifest.events) }
    }

    fun validate() {
        manifest.validate()
        require(manifest.kind == "lua") { "Source packages must use kind lua" }
        require(sources.keys == manifest.files.toSet()) { "Imported resources do not match the manifest" }
        val manifestSize = pluginUtf8Bytes(manifest.toJson()).size
        require(manifestSize <= MAX_PLUGIN_MANIFEST_BYTES) { "Plugin manifest is too large" }
        var total = manifestSize.toLong()
        sources.values.forEach { total += pluginUtf8Bytes(it).size }
        require(total <= MAX_PLUGIN_PACKAGE_BYTES) { "Plugin package exceeds $MAX_PLUGIN_PACKAGE_BYTES bytes" }
        if (manifest.ui != null) requireNotNull(page)
    }
}
