package cafe.ivory.dejavu.data

import org.json.JSONObject
import org.json.JSONTokener
import org.json.JSONException

const val PLUGIN_ROUTER_FILE = "router.json"
const val MAX_PLUGIN_ROUTER_BYTES = 32 * 1024

/** Configuration metadata is consumed by Router, never by injected plugin code. */
internal object PluginRouterConfig {
    fun validate(source: String, pluginId: String) {
        try {
            validateSchema(source, pluginId)
        } catch (_: JSONException) {
            // org.json type errors may include the full supplied object, including tokens.
            throw IllegalArgumentException("Invalid plugin router.json schema")
        }
    }

    private fun validateSchema(source: String, pluginId: String) {
        validatePluginId(pluginId)
        require(pluginUtf8Bytes(source).size <= MAX_PLUGIN_ROUTER_BYTES) { "Plugin router.json is too large" }
        val root = strictPluginJsonObject(source)
        root.onlyKeys("schemaVersion", "services", "upstreams", "default_upstream", "routes")
        require(root.integer("schemaVersion", 1, 1) == 1) { "Unsupported plugin router version" }
        if (root.has("services")) {
            val services = root.getJSONArray("services")
            require(services.length() in 1..8) { "Declare between 1 and 8 Router services" }
            val names = (0 until services.length()).map { index ->
                val name = services.get(index) as? String
                require(name?.matches(Regex("[A-Za-z0-9_.-]{1,128}")) == true) { "Invalid Router service name" }
                name
            }
            require(names.distinct().size == names.size) { "Duplicate Router service name" }
        }
        val upstreams = root.getJSONArray("upstreams")
        require(upstreams.length() in 1..8) { "Declare between 1 and 8 Router upstreams" }
        val ids = (0 until upstreams.length()).map { index ->
            val upstream = upstreams.getJSONObject(index)
            upstream.onlyKeys(
                "id", "host", "port", "device_id", "device_token",
                "protocol", "tls", "tls_ca_file", "tls_sni", "tls_insecure",
            )
            val id = upstream.text("id", 64)
            require(id.matches(Regex("[a-z][a-z0-9_-]{0,63}"))) { "Invalid Router upstream identifier" }
            upstream.text("host", 253)
            upstream.integer("port", 1, 65535)
            if (upstream.has("device_id")) upstream.text("device_id", 128)
            if (upstream.has("device_token")) upstream.text("device_token", 4096, empty = true)
            if (upstream.has("protocol")) {
                val protocol = upstream.text("protocol", 16)
                require(protocol == "v1" || protocol == "v2") { "Unsupported Router protocol" }
            }
            if (upstream.has("tls")) require(upstream.get("tls") is Boolean) { "Router tls must be a boolean" }
            if (upstream.has("tls_insecure")) {
                require(upstream.get("tls_insecure") is Boolean) { "Router tls_insecure must be a boolean" }
            }
            if (upstream.has("tls_ca_file")) {
                val caFile = upstream.text("tls_ca_file", 1024, empty = true)
                require(caFile.isEmpty() || caFile.startsWith("/")) { "Router tls_ca_file must be an absolute path" }
            }
            if (upstream.has("tls_sni")) upstream.text("tls_sni", 253, empty = true)
            id
        }
        require(ids.distinct().size == ids.size) { "Duplicate Router upstream identifier" }
        require(root.text("default_upstream", 64) in ids) { "Unknown default Router upstream" }
        if (root.has("routes")) {
            val routes = root.getJSONObject("routes")
            require(routes.length() <= 512) { "Too many Router route bindings" }
            routes.keys().forEach { route ->
                checkedText(route, 128, false)
                require(routes.text(route, 64) in ids) { "Unknown Router route upstream" }
            }
        }
    }

    private fun JSONObject.onlyKeys(vararg allowed: String) {
        require(keys().asSequence().all { it in allowed }) { "Unexpected plugin Router field" }
    }

    private fun JSONObject.text(key: String, maximum: Int, empty: Boolean = false): String {
        val value = get(key) as? String ?: throw IllegalArgumentException("Router $key must be a string")
        checkedText(value, maximum, empty)
        return value
    }

    private fun checkedText(value: String, maximum: Int, empty: Boolean) {
        require((empty || value.isNotEmpty()) && pluginUtf8Bytes(value).size <= maximum &&
            value.none { it.code < 32 || it.code == 127 }) { "Invalid Router text field" }
    }

    private fun JSONObject.integer(key: String, minimum: Int, maximum: Int): Int {
        val value = get(key)
        require(value is Number && value.toDouble().isFinite() &&
            value.toDouble() in minimum.toDouble()..maximum.toDouble() && value.toDouble() == value.toInt().toDouble()) {
            "Invalid Router $key integer"
        }
        return value.toInt()
    }
}

internal fun validatePluginRouterResource(manifest: PluginPackageManifest, bytes: ByteArray) {
    require(manifest.router != null) { "Plugin does not declare Router metadata" }
    require(bytes.size <= MAX_PLUGIN_ROUTER_BYTES) { "Plugin router.json is too large" }
    PluginRouterConfig.validate(decodePluginUtf8(bytes), manifest.id)
}

/** Android's JSONTokener accepts duplicate keys, comments, unquoted values and trailing input.
 * Validate bounded standard JSON first so desktop imports and Android agree with the daemon. */
internal fun strictPluginJsonObject(source: String): JSONObject {
    var cursor = 0
    fun whitespace() {
        while (cursor < source.length && source[cursor] in " \t\r\n") cursor++
    }
    fun character(expected: Char) {
        require(cursor < source.length && source[cursor++] == expected) { "Invalid plugin JSON structure" }
    }
    fun string(): String {
        val start = cursor
        character('"')
        while (cursor < source.length) {
            val next = source[cursor++]
            require(next.code >= 32) { "Invalid plugin JSON string" }
            if (next == '"') {
                val decoded = JSONTokener(source.substring(start, cursor)).nextValue() as String
                pluginUtf8Bytes(decoded)
                return decoded
            }
            if (next == '\\') {
                require(cursor < source.length) { "Incomplete plugin JSON escape" }
                val escape = source[cursor++]
                require(escape in "\"\\/bfnrtu") { "Invalid plugin JSON escape" }
                if (escape == 'u') repeat(4) {
                    require(cursor < source.length && source[cursor++] in "0123456789abcdefABCDEF") {
                        "Invalid plugin JSON Unicode escape"
                    }
                }
            }
        }
        throw IllegalArgumentException("Incomplete plugin JSON string")
    }
    fun value(depth: Int) {
        require(depth <= 16) { "Plugin JSON is nested too deeply" }
        whitespace()
        require(cursor < source.length) { "Incomplete plugin JSON" }
        when (source[cursor]) {
            '{' -> {
                cursor++
                whitespace()
                val keys = mutableSetOf<String>()
                if (cursor < source.length && source[cursor] == '}') { cursor++; return }
                while (true) {
                    whitespace()
                    require(keys.add(string())) { "Duplicate plugin JSON field" }
                    whitespace()
                    character(':')
                    value(depth + 1)
                    whitespace()
                    if (cursor < source.length && source[cursor] == '}') { cursor++; return }
                    character(',')
                }
            }
            '[' -> {
                cursor++
                whitespace()
                if (cursor < source.length && source[cursor] == ']') { cursor++; return }
                while (true) {
                    value(depth + 1)
                    whitespace()
                    if (cursor < source.length && source[cursor] == ']') { cursor++; return }
                    character(',')
                }
            }
            '"' -> string()
            else -> {
                val start = cursor
                while (cursor < source.length && source[cursor] !in ",]} \t\r\n") cursor++
                val token = source.substring(start, cursor)
                require(token in setOf("true", "false", "null") ||
                    token.matches(Regex("-?(0|[1-9][0-9]*)(\\.[0-9]+)?([eE][+-]?[0-9]+)?"))) {
                    "Invalid plugin JSON value"
                }
            }
        }
    }
    value(1)
    whitespace()
    require(cursor == source.length) { "Trailing plugin JSON input" }
    return JSONObject(source)
}
