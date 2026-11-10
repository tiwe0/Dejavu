package cafe.ivory.dejavu.data

import org.json.JSONObject
import org.json.JSONTokener

private const val MAX_PAGE_BYTES = 32 * 1024
private const val MAX_PAGE_BLOCKS = 32
private const val MAX_PAGE_DEPTH = 4
private const val MAX_PAGE_ACTIONS = 8
private const val MAX_PAGE_INFO_ROWS = 16
private const val MAX_PAGE_EVENTS = 50
private val pageCallPattern = Regex("[a-z][a-z0-9_]{0,63}")

internal fun validatePluginPageCall(name: String) {
    require(name.matches(pageCallPattern)) { "Invalid plugin page call" }
}

data class PluginPageText(val en: String, val zh: String? = null)

sealed interface PluginPageBlock {
    data class Text(val text: PluginPageText) : PluginPageBlock
    data object Status : PluginPageBlock
    data class Button(val label: PluginPageText, val action: String, val call: String? = null) : PluginPageBlock
    data class Input(val label: PluginPageText) : PluginPageBlock
    data class Events(val limit: Int) : PluginPageBlock
}

data class PluginPageAction(val id: String, val label: PluginPageText)

data class PluginPageInfoRow(val label: PluginPageText, val value: PluginPageText)

sealed interface PluginPagePreset {
    val actions: List<PluginPageAction>

    data class Monitor(val eventLimit: Int, override val actions: List<PluginPageAction>) : PluginPagePreset
    data class Controls(val rows: List<PluginPageInfoRow>, override val actions: List<PluginPageAction>) : PluginPagePreset
}

data class PluginPage(
    val title: PluginPageText,
    val blocks: List<PluginPageBlock>,
    val preset: PluginPagePreset? = null,
) {
    val eventLimit: Int?
        get() = (preset as? PluginPagePreset.Monitor)?.eventLimit
            ?: blocks.firstNotNullOfOrNull { (it as? PluginPageBlock.Events)?.limit }

    fun hasAction(id: String): Boolean = preset?.actions?.any { it.id == id }
        ?: blocks.any { it is PluginPageBlock.Button && it.action == "call" && it.call == id }

    companion object {
        internal fun parse(source: String, events: PluginEventSpec?): PluginPage {
            require(pluginUtf8Bytes(source).size <= MAX_PAGE_BYTES) { "Plugin page exceeds 32 KiB" }
            requireBoundedPageNesting(source)
            val input = JSONTokener(source)
            val json = input.nextValue() as? JSONObject
                ?: throw IllegalArgumentException("Plugin page must be a JSON object")
            require(input.nextClean() == '\u0000') { "Plugin page has trailing data" }
            return when (json.pageInteger("version")) {
                1L -> parseBlocks(json, events)
                2L -> parsePreset(json, events)
                else -> throw IllegalArgumentException("Unsupported plugin page version")
            }
        }

        private fun parseBlocks(json: JSONObject, events: PluginEventSpec?): PluginPage {
            json.allowPageKeys("version", "title", "blocks")
            val array = json.getJSONArray("blocks")
            require(array.length() in 1..MAX_PAGE_BLOCKS) { "Declare between 1 and $MAX_PAGE_BLOCKS plugin page blocks" }
            val blocks = List(array.length()) { index -> parseBlock(array.getJSONObject(index), events) }
            require(blocks.count { it is PluginPageBlock.Input } <= 1) { "Plugin page allows one listener input" }
            require(blocks.count { it is PluginPageBlock.Events } <= 1) { "Plugin page allows one event list" }
            require(blocks.none { it is PluginPageBlock.Input } || blocks.any { it is PluginPageBlock.Events }) {
                "Listener input requires an event list"
            }
            return PluginPage(json.pageText("title", 120), blocks)
        }

        private fun parsePreset(json: JSONObject, events: PluginEventSpec?): PluginPage {
            val presetName = json.pageString("preset")
            when (presetName) {
                "monitor" -> json.allowPageKeys("version", "title", "preset", "eventLimit", "actions")
                "controls" -> json.allowPageKeys("version", "title", "preset", "rows", "actions")
                else -> throw IllegalArgumentException("Unsupported plugin page preset")
            }
            val actions = json.pageObjects("actions", MAX_PAGE_ACTIONS) {
                allowPageKeys("id", "label")
                PluginPageAction(pageString("id").also(::validatePluginPageCall), pageText("label", 120))
            }
            require(actions.map { it.id }.toSet().size == actions.size) { "Plugin page action IDs must be unique" }
            val preset = if (presetName == "monitor") {
                val limit = if (json.has("eventLimit")) json.pageInteger("eventLimit") else MAX_PAGE_EVENTS.toLong()
                require(events != null && limit in 1..MAX_PAGE_EVENTS) {
                    "Monitor preset requires declared events and a limit of 1–50"
                }
                PluginPagePreset.Monitor(limit.toInt(), actions)
            } else {
                val rows = json.pageObjects("rows", MAX_PAGE_INFO_ROWS) {
                    allowPageKeys("label", "value")
                    PluginPageInfoRow(pageText("label", 120), pageText("value", 2048))
                }
                PluginPagePreset.Controls(rows, actions)
            }
            return PluginPage(json.pageText("title", 120), emptyList(), preset)
        }

        private fun parseBlock(json: JSONObject, events: PluginEventSpec?): PluginPageBlock =
            when (json.pageString("type")) {
                "text" -> {
                    json.allowPageKeys("type", "text")
                    PluginPageBlock.Text(json.pageText("text", 2048))
                }
                "status" -> {
                    json.allowPageKeys("type", "bind")
                    require(json.pageString("bind") == "runtime.state") { "Unsupported plugin status binding" }
                    PluginPageBlock.Status
                }
                "button" -> {
                    json.allowPageKeys("type", "label", "action", "call")
                    val action = json.pageString("action")
                    require(action in setOf("check", "start", "stop", "call")) { "Unsupported plugin page action" }
                    val call = if (action == "call") {
                        json.pageString("call").also(::validatePluginPageCall)
                    } else {
                        require(!json.has("call")) { "Only call buttons can name a plugin action" }
                        null
                    }
                    PluginPageBlock.Button(json.pageText("label", 120), action, call)
                }
                "input" -> {
                    json.allowPageKeys("type", "label", "bind")
                    require(json.pageString("bind") == "events.port" && events?.usesSocket == true) {
                        "Listener input requires hook socket events"
                    }
                    PluginPageBlock.Input(json.pageText("label", 120))
                }
                "events" -> {
                    json.allowPageKeys("type", "limit")
                    val limit = json.pageInteger("limit")
                    require(events != null && limit in 1..MAX_PAGE_EVENTS) { "Plugin event list requires declared events and a limit of 1–50" }
                    PluginPageBlock.Events(limit.toInt())
                }
                else -> throw IllegalArgumentException("Unsupported plugin page block")
            }
    }
}

private fun JSONObject.allowPageKeys(vararg keys: String) {
    require(this.keys().asSequence().all { it in keys }) { "Unexpected plugin page field" }
}

private fun <T> JSONObject.pageObjects(key: String, maximum: Int, parse: JSONObject.() -> T): List<T> {
    if (!has(key)) return emptyList()
    val array = getJSONArray(key)
    require(array.length() <= maximum) { "Plugin page $key allows at most $maximum entries" }
    return List(array.length()) { index -> array.getJSONObject(index).parse() }
}

private fun JSONObject.pageString(key: String): String =
    get(key) as? String ?: throw IllegalArgumentException("Plugin page $key must be a string")

private fun JSONObject.pageInteger(key: String): Long {
    val value = get(key)
    require(value is Int || value is Long) { "Plugin page $key must be an integer" }
    return (value as Number).toLong()
}

private fun JSONObject.pageText(key: String, limit: Int): PluginPageText {
    fun checked(text: String): String {
        require(text.isNotBlank() && text.length <= limit) { "Invalid plugin page $key text" }
        pluginUtf8Bytes(text)
        return text
    }
    return when (val value = get(key)) {
        is String -> PluginPageText(checked(value))
        is JSONObject -> {
            value.allowPageKeys("en", "zh")
            PluginPageText(checked(value.pageString("en")), value.opt("zh")?.let {
                checked(value.pageString("zh"))
            })
        }
        else -> throw IllegalArgumentException("Plugin page $key must be text")
    }
}

// Bound the recursive JSON parser before giving it plugin-supplied input. Android's
// JSONTokener accepts comments and single quotes, which would change string boundaries.
private fun requireBoundedPageNesting(source: String) {
    var depth = 0
    var quoted = false
    var escaped = false
    for (char in source) {
        if (quoted) {
            require(char.code >= 0x20) { "Invalid plugin page string" }
            when {
                escaped -> {
                    require(char in "\"\\/bfnrtu") { "Invalid plugin page escape" }
                    escaped = false
                }
                char == '\\' -> escaped = true
                char == '"' -> quoted = false
            }
        } else {
            when (char) {
                '"' -> quoted = true
                '{', '[' -> require(++depth <= MAX_PAGE_DEPTH) { "Plugin page is nested too deeply" }
                '}', ']' -> require(--depth >= 0) { "Invalid plugin page structure" }
                ':', ',', '-', '.', ' ', '\t', '\r', '\n',
                in '0'..'9', 'e', 'E', '+', 't', 'r', 'u', 'f', 'a', 'l', 's', 'n' -> Unit
                else -> throw IllegalArgumentException("Invalid plugin page JSON")
            }
        }
    }
    require(!quoted && depth == 0) { "Incomplete plugin page JSON" }
}
