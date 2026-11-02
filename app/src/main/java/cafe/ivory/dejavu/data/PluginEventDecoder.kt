package cafe.ivory.dejavu.data

import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.nio.charset.StandardCharsets

/** Reassembles live plugin frames in memory; completed message bodies are never retained. */
class PluginEventDecoder(private val marker: String, private val session: String) {
    private data class Pending(
        val created: Long,
        val chunks: Array<String?>,
        var size: Int = 0,
        var received: Int = 0,
    )

    private val pending = linkedMapOf<String, Pending>()
    // Only UUIDs and timestamps are kept after completion, rejection or expiry.
    private val retired = linkedMapOf<String, Long>()

    private var dropped = 0

    @get:Synchronized
    val droppedCount: Int
        get() = dropped

    init {
        require(marker.length in 1..64 && marker.all { it in '!'..'~' }) {
            "invalid event marker"
        }
        require(SESSION_PATTERN.matches(session)) { "invalid event session" }
    }

    @Synchronized
    fun clear() {
        pending.clear()
        retired.clear()
        dropped = 0
    }

    @Synchronized
    fun feed(line: String, nowMillis: Long = System.nanoTime() / 1_000_000): String? {
        expire(nowMillis)
        // Examine only a bounded prefix of an oversized line to identify its
        // session and invalidate its event without splitting an arbitrary log.
        val words = line.take(MAX_FRAME_CHARS).trim().split(WHITESPACE, limit = 8)
        if (words.size < 3 || words[0] != marker || words[2] != session) return null
        if (words[1] == "DROP") {
            countDrop()
            return null
        }
        if (words[1] != "DATA") return null
        val key = words.getOrNull(3)?.takeIf { KEY_PATTERN.matches(it) }
        if (line.length > MAX_FRAME_CHARS || words.size != 7 || key == null) {
            reject(key, nowMillis)
            return null
        }
        if (key in retired) return null
        val indexText = words[4]
        val countText = words[5]
        val index = indexText.takeIf { PART_PATTERN.matches(it) }?.toIntOrNull()
        val count = countText.takeIf { PART_PATTERN.matches(it) }?.toIntOrNull()
        val chunk = words[6]
        if (count == null || index == null || count !in 1..MAX_PARTS || index !in 0 until count ||
            chunk.length !in 1..MAX_CHUNK || !ENCODED_PATTERN.matches(chunk)
        ) {
            reject(key, nowMillis)
            return null
        }
        val item = pending[key] ?: run {
            if (pending.size >= MAX_PENDING) {
                reject(pending.keys.first(), nowMillis)
            }
            Pending(nowMillis, arrayOfNulls(count)).also { pending[key] = it }
        }
        if (item.chunks.size != count) {
            reject(key, nowMillis)
            return null
        }
        val previous = item.chunks[index]
        if (previous != null) {
            if (previous != chunk) reject(key, nowMillis)
            return null
        }
        item.chunks[index] = chunk
        item.size += chunk.length
        item.received += 1
        if (item.size > MAX_ENCODED) {
            reject(key, nowMillis)
            return null
        }
        if (item.received != count) return null
        pending.remove(key)
        retire(key, nowMillis)
        return try {
            decode(item.chunks.joinToString(separator = "") { requireNotNull(it) })
        } catch (_: IllegalArgumentException) {
            countDrop()
            null
        } catch (_: java.nio.charset.CharacterCodingException) {
            countDrop()
            null
        }
    }

    private fun expire(now: Long) {
        val retiredIterator = retired.entries.iterator()
        while (retiredIterator.hasNext()) {
            if (now - retiredIterator.next().value >= RETIRED_TTL_MILLIS) retiredIterator.remove()
        }
        val iterator = pending.entries.iterator()
        while (iterator.hasNext()) {
            val (key, item) = iterator.next()
            if (now - item.created >= PENDING_TTL_MILLIS) {
                iterator.remove()
                retire(key, now)
                countDrop()
            }
        }
    }

    private fun reject(key: String?, now: Long) {
        if (key != null) {
            pending.remove(key)
            retire(key, now)
        }
        countDrop()
    }

    private fun retire(key: String, now: Long) {
        retired[key] = now
        if (retired.size > MAX_RETIRED) retired.remove(retired.keys.first())
    }

    private fun countDrop() {
        if (dropped < Int.MAX_VALUE) dropped += 1
    }

    private fun decode(encoded: String): String {
        val bytes = ByteArrayOutputStream(encoded.length)
        var index = 0
        while (index < encoded.length) {
            when (val character = encoded[index]) {
                '%' -> {
                    require(index + 2 < encoded.length) { "truncated percent escape" }
                    val high = encoded[index + 1].digitToIntOrNull(16)
                    val low = encoded[index + 2].digitToIntOrNull(16)
                    require(high != null && low != null) { "invalid percent escape" }
                    bytes.write((high shl 4) or low)
                    index += 3
                }
                '+' -> {
                    bytes.write(' '.code)
                    index += 1
                }
                else -> {
                    bytes.write(character.code)
                    index += 1
                }
            }
        }
        return StandardCharsets.UTF_8.newDecoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT)
            .decode(ByteBuffer.wrap(bytes.toByteArray()))
            .toString()
    }

    private companion object {
        const val MAX_CHUNK = 1600
        const val MAX_PARTS = 113
        const val MAX_ENCODED = 180000
        const val MAX_PENDING = 32
        const val MAX_FRAME_CHARS = 2048
        const val MAX_RETIRED = 4096
        const val PENDING_TTL_MILLIS = 5000L
        const val RETIRED_TTL_MILLIS = 60000L
        val WHITESPACE = Regex("\\s+")
        val SESSION_PATTERN = Regex("[0-9a-f]{16}")
        val KEY_PATTERN = Regex("[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}")
        val PART_PATTERN = Regex("[0-9]{1,3}")
        val ENCODED_PATTERN = Regex("[A-Za-z0-9.*_+%\\-]+")
    }
}
