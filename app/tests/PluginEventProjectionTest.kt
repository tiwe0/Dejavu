package cafe.ivory.dejavu.data

import org.json.JSONObject

fun runPluginEventProjectionTests() {
    val spec = PluginEventSpec(
        tag = "SensorEvents", marker = "SENSOR_V1",
        titleField = "source", contentField = "payload", metaField = "time",
        dedupFields = listOf("sensor", "sequence"),
        dedupFallbackFields = listOf("source", "time"),
        dedupIgnoreValues = listOf("", "pending"),
    )
    val event = JSONObject("""{"sensor":"north","sequence":42,"source":"温度😀","payload":"23°C","time":"10:30"}""")
    val projected = projectPluginEvent(event, spec, "Sensor plugin")
    check(projected.item == LivePluginEvent("温度😀", "23°C", "10:30"))
    check(projected.dedupKey?.matches(Regex("[0-9a-f]{64}")) == true)
    check(projectPluginEvent(JSONObject(event.toString()).put("payload", "updated"), spec, "other").dedupKey == projected.dedupKey)
    check(projectPluginEvent(JSONObject(event.toString()).put("sequence", 43), spec, "other").dedupKey != projected.dedupKey)

    val fallback = JSONObject(event.toString()).put("sequence", "pending")
    val fallbackKey = projectPluginEvent(fallback, spec, "Sensor plugin").dedupKey
    check(fallbackKey != null && fallbackKey != projected.dedupKey)
    check(projectPluginEvent(JSONObject(fallback.toString()).put("sequence", JSONObject.NULL), spec, "other").dedupKey == fallbackKey)
    check(projectPluginEvent(JSONObject(fallback.toString()).put("sequence", ""), spec, "other").dedupKey == fallbackKey)
    check(projectPluginEvent(JSONObject(fallback.toString()).put("source", JSONObject.NULL), spec, "other").dedupKey == null)
    check(projectPluginEvent(JSONObject(fallback.toString()).put("time", ""), spec, "other").dedupKey == null)

    val compound = spec.copy(dedupFields = listOf("source", "payload"), dedupFallbackFields = emptyList())
    val left = projectPluginEvent(JSONObject("""{"source":"a:b","payload":"c"}"""), compound, "x")
    val right = projectPluginEvent(JSONObject("""{"source":"a","payload":"b:c"}"""), compound, "x")
    check(left.dedupKey != right.dedupKey)
    check(projectPluginEvent(JSONObject().put("payload", "a:b").put("source", "c"), compound.copy(
        dedupFields = listOf("payload", "source"),
    ), "x").dedupKey != left.dedupKey)

    val defaults = PluginEventSpec("Generic", "GENERIC_V1")
    val ordinary = projectPluginEvent(JSONObject("""{"title":"status","content":"ready","meta":7}"""), defaults, "Plugin")
    check(ordinary.item == LivePluginEvent("status", "ready", "7") && ordinary.dedupKey == null)
    val missing = JSONObject().put("unmapped", "detail")
    check(projectPluginEvent(missing, defaults, "Plugin").item == LivePluginEvent("Plugin", missing.toString(), ""))
    check(projectPluginEvent(JSONObject().put("title", JSONObject.NULL).put("content", JSONObject.NULL), defaults, "Plugin").item.title == "Plugin")
    val bounded = projectPluginEvent(JSONObject().put("source", "t".repeat(2000))
        .put("payload", "b".repeat(20000)).put("time", "m".repeat(2000)), spec, "Plugin")
    check(bounded.item.title.length == 1024 && bounded.item.content.length == 16384 && bounded.item.meta.length == 1024)
    check(projectPluginEvent(JSONObject(), defaults, "f".repeat(2000)).item.title.length == 1024)

    val manifest = PluginPackageManifest("sensor", "Sensor plugin", "1", entry = "app.lua",
        files = listOf("app.lua"), events = spec)
    check(PluginPackageManifest.parse(manifest.toJson()).events == spec)
    val original = JSONObject(manifest.toJson())
    fun rejected(events: JSONObject) {
        check(runCatching { PluginPackageManifest.parse(JSONObject(original.toString()).put("events", events).toString()) }.isFailure)
    }
    for (field in listOf("", "1source", "source key", "source/key", "x".repeat(65))) {
        check(runCatching { spec.copy(titleField = field).validate() }.isFailure)
    }
    check(runCatching { spec.copy(dedupFields = listOf("sensor", "sensor")).validate() }.isFailure)
    check(runCatching { spec.copy(dedupFields = (1..9).map { "field$it" }).validate() }.isFailure)
    check(runCatching { spec.copy(dedupIgnoreValues = listOf("x", "x")).validate() }.isFailure)
    check(runCatching { spec.copy(dedupIgnoreValues = listOf("x".repeat(129))).validate() }.isFailure)
    check(runCatching { spec.copy(dedupIgnoreValues = (1..9).map { "$it" }).validate() }.isFailure)
    rejected(JSONObject(original.getJSONObject("events").toString()).put("dedupFields", "sensor"))
    rejected(JSONObject(original.getJSONObject("events").toString()).put("dedupFallbackFields", org.json.JSONArray().put(7)))
    rejected(JSONObject(original.getJSONObject("events").toString()).put("contentField", 7))
    println("OK: manifest-driven generic event display, bounded fields, identities and fallback")
}
