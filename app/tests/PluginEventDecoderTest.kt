package cafe.ivory.dejavu.data

import java.net.URLEncoder

private const val EVENT_TEST_MARKER = "DEJAVU_TEST_EVENTS"
private const val EVENT_TEST_SESSION = "0123456789abcdef"

private fun eventTestUuid(number: Int) =
    "00000000-0000-0000-0000-" + number.toString(16).padStart(12, '0')

private fun eventTestFrame(
    key: Int,
    index: Int,
    count: Int,
    chunk: String,
    session: String = EVENT_TEST_SESSION,
) = "$EVENT_TEST_MARKER DATA $session ${eventTestUuid(key)} $index $count $chunk"

private fun eventTestDecoder() = PluginEventDecoder(EVENT_TEST_MARKER, EVENT_TEST_SESSION)

fun runPluginEventDecoderTests() {
    val payload = """{"content":"中文😀 +%","title":"测试"}"""
    val encoded = URLEncoder.encode(payload, "UTF-8")
    val split = encoded.indexOf("%E4") + 1
    val chunks = listOf(encoded.take(split), encoded.substring(split, split + 2), encoded.drop(split + 2))
    val unicode = eventTestDecoder()
    check(unicode.feed(eventTestFrame(1, 2, 3, chunks[2]), 0) == null)
    check(unicode.feed(eventTestFrame(1, 0, 3, chunks[0]), 1) == null)
    check(unicode.feed(eventTestFrame(1, 0, 3, chunks[0]), 2) == null)
    check(unicode.feed(eventTestFrame(1, 1, 3, chunks[1]), 3) == payload)
    check(unicode.droppedCount == 0)
    // Completed UUIDs are suppressed without retaining their decoded content.
    chunks.forEachIndexed { index, chunk ->
        check(unicode.feed(eventTestFrame(1, index, 3, chunk), 4) == null)
    }
    check(unicode.droppedCount == 0)

    val isolated = eventTestDecoder()
    check(isolated.feed(eventTestFrame(2, 0, 1, "%7B%7D", "fedcba9876543210"), 0) == null)
    check(isolated.feed("OTHER DATA $EVENT_TEST_SESSION ${eventTestUuid(2)} 0 1 x", 0) == null)
    check(isolated.feed("$EVENT_TEST_MARKER DROP fedcba9876543210 size", 0) == null)
    check(isolated.droppedCount == 0)
    check(isolated.feed("$EVENT_TEST_MARKER DROP $EVENT_TEST_SESSION size", 0) == null)
    check(isolated.droppedCount == 1)

    val malformed = listOf(
        "$EVENT_TEST_MARKER DATA $EVENT_TEST_SESSION missing-fields",
        "$EVENT_TEST_MARKER DATA $EVENT_TEST_SESSION invalid-uuid 0 1 x",
        eventTestFrame(3, 0, 0, "x"),
        eventTestFrame(3, 0, 114, "x"),
        eventTestFrame(3, 1, 1, "x"),
        eventTestFrame(3, 0, 1, "x").replace(" 0 1 ", " -0 1 "),
        eventTestFrame(3, 0, 1, "x").replace(" 0 1 ", " ０ 1 "),
        eventTestFrame(3, 0, 1, "x").replace(" 0 1 ", " 0000 1 "),
        eventTestFrame(3, 0, 1, "x") + " trailing",
        eventTestFrame(3, 0, 1, "x".repeat(1601)),
        eventTestFrame(3, 0, 1, "x".repeat(5000)),
        eventTestFrame(3, 0, 1, "bad!"),
        eventTestFrame(3, 0, 1, "中文"),
        eventTestFrame(3, 0, 1, "%"),
        eventTestFrame(3, 0, 1, "%0"),
        eventTestFrame(3, 0, 1, "%GG"),
        eventTestFrame(3, 0, 1, "%C3%28"),
        eventTestFrame(3, 0, 1, "%ED%A0%80"),
        eventTestFrame(3, 0, 1, "%F4%90%80%80"),
    )
    for (frame in malformed) {
        val decoder = eventTestDecoder()
        check(decoder.feed(frame, 0) == null) { "accepted invalid frame: $frame" }
        check(decoder.droppedCount == 1) { "invalid frame was not counted" }
    }

    val conflict = eventTestDecoder()
    check(conflict.feed(eventTestFrame(4, 0, 2, "%7B"), 0) == null)
    check(conflict.feed(eventTestFrame(4, 0, 2, "%5B"), 1) == null)
    check(conflict.droppedCount == 1)
    check(conflict.feed(eventTestFrame(4, 1, 2, "%7D"), 2) == null)
    check(conflict.feed(eventTestFrame(4, 0, 2, "%7B"), 3) == null)
    check(conflict.droppedCount == 1)
    val inconsistent = eventTestDecoder()
    check(inconsistent.feed(eventTestFrame(5, 0, 2, "%7B"), 0) == null)
    check(inconsistent.feed(eventTestFrame(5, 1, 3, "%7D"), 1) == null)
    check(inconsistent.droppedCount == 1)

    val capacity = eventTestDecoder()
    for (key in 1..33) {
        check(capacity.feed(eventTestFrame(key, 0, 2, "%7B"), 0) == null)
    }
    check(capacity.droppedCount == 1)
    check(capacity.feed(eventTestFrame(1, 1, 2, "%7D"), 1) == null)
    check(capacity.droppedCount == 1)
    check(capacity.feed(eventTestFrame(33, 1, 2, "%7D"), 1) == "{}")

    val timeout = eventTestDecoder()
    check(timeout.feed(eventTestFrame(6, 0, 2, "%7B"), 10) == null)
    check(timeout.feed(eventTestFrame(6, 0, 2, "%7B"), 5009) == null)
    check(timeout.droppedCount == 0) // Retransmission must not extend the TTL.
    check(timeout.feed(eventTestFrame(6, 1, 2, "%7D"), 5010) == null)
    check(timeout.droppedCount == 1)
    check(timeout.feed(eventTestFrame(6, 0, 2, "%7B"), 5011) == null)
    timeout.clear()
    check(timeout.droppedCount == 0)
    check(timeout.feed(eventTestFrame(6, 1, 2, "%7D"), 5020) == null)
    check(timeout.feed(eventTestFrame(6, 0, 2, "%7B"), 5021) == "{}")
    timeout.clear()
    check(timeout.feed(eventTestFrame(6, 0, 1, "%7B%7D"), 5022) == "{}")
    val clearPartial = eventTestDecoder()
    check(clearPartial.feed(eventTestFrame(9, 0, 2, "%7B"), 0) == null)
    clearPartial.clear()
    check(clearPartial.feed(eventTestFrame(9, 1, 2, "%7D"), 1) == null)
    check(clearPartial.feed(eventTestFrame(9, 0, 2, "%7B"), 2) == "{}")

    val retiredLimit = eventTestDecoder()
    for (key in 1..4097) {
        check(retiredLimit.feed(eventTestFrame(key, 0, 1, "%7B%7D"), 0) == "{}")
    }
    check(retiredLimit.feed(eventTestFrame(1, 0, 1, "%7B%7D"), 1) == "{}")
    check(retiredLimit.feed(eventTestFrame(1, 0, 1, "%7B%7D"), 60001) == "{}")
    check(retiredLimit.droppedCount == 0)

    val exactLimit = eventTestDecoder()
    var complete: String? = null
    repeat(113) { index ->
        val chunk = "a".repeat(if (index == 112) 800 else 1600)
        complete = exactLimit.feed(eventTestFrame(7, index, 113, chunk), index.toLong())
        if (index != 112) check(complete == null)
    }
    check(complete == "a".repeat(180000))
    check(exactLimit.droppedCount == 0)
    val tooLarge = eventTestDecoder()
    repeat(113) { index ->
        val chunk = "a".repeat(if (index == 112) 801 else 1600)
        check(tooLarge.feed(eventTestFrame(8, index, 113, chunk), index.toLong()) == null)
    }
    check(tooLarge.droppedCount == 1)

    check(runCatching { PluginEventDecoder(EVENT_TEST_MARKER, "invalid") }.isFailure)
    check(runCatching { PluginEventDecoder("bad marker", EVENT_TEST_SESSION) }.isFailure)
    println("OK: plugin event UTF-8 fragments, sessions, duplicates, limits, TTL and clearing")
}
