package cafe.ivory.dejavu.data

import java.io.BufferedInputStream
import java.io.Closeable
import java.io.DataInputStream
import java.io.EOFException
import java.io.IOException
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketTimeoutException
import java.util.concurrent.TimeUnit

internal fun validPluginListenPort(port: Int) = port == 0 || port in 1024..65535

private const val MAX_HOOK_PAYLOAD = 4 * 1024 * 1024
private const val MAX_HOOK_FRAME = 2 + 65535 + MAX_HOOK_PAYLOAD
private const val MAX_VIEW_PAYLOAD = 64 * 1024
private const val MAX_VIEW_TOPIC = 128

internal class PluginHookEventSocket(port: Int) : Closeable {
    private val server = ServerSocket()
    private val lock = Any()
    private var client: Socket? = null
    @Volatile private var stopped = false

    init {
        try {
            require(validPluginListenPort(port)) { "Invalid listener port" }
            server.reuseAddress = true
            server.soTimeout = 250
            server.bind(InetSocketAddress(InetAddress.getByAddress(byteArrayOf(127, 0, 0, 1)), port), 4)
        } catch (error: Exception) {
            runCatching { server.close() }
            throw error
        }
    }

    val listenPort: Int get() = server.localPort

    fun readEvents(topic: String, onConnected: () -> Unit, onPayload: (String) -> Unit) {
        val socket = accept() ?: return
        socket.use {
            synchronized(lock) {
                if (stopped) return
                client = socket
            }
            try {
                onConnected()
                val input = DataInputStream(BufferedInputStream(socket.getInputStream()))
                while (!stopped) {
                    val first = input.read()
                    if (first == -1) return
                    val size = (first shl 24) or (input.readUnsignedByte() shl 16) or
                        (input.readUnsignedByte() shl 8) or input.readUnsignedByte()
                    if (size !in 3..MAX_HOOK_FRAME) throw IOException("Invalid hook event frame length")
                    val topicSize = input.readUnsignedShort()
                    if (topicSize !in 1..(size - 2)) throw IOException("Invalid hook event topic length")
                    val payloadSize = size - 2 - topicSize
                    if (payloadSize > MAX_HOOK_PAYLOAD) throw IOException("Invalid hook event payload length")
                    if (topicSize > MAX_VIEW_TOPIC || payloadSize > MAX_VIEW_PAYLOAD) {
                        input.discard(size - 2)
                        continue
                    }
                    val topicBytes = ByteArray(topicSize).also(input::readFully)
                    val frameTopic = runCatching { decodePluginUtf8(topicBytes) }.getOrNull()
                    if (frameTopic != topic) {
                        input.discard(payloadSize)
                        continue
                    }
                    val payload = ByteArray(payloadSize).also(input::readFully)
                    if (stopped) return
                    val text = runCatching { decodePluginUtf8(payload) }.getOrNull() ?: continue
                    onPayload(text)
                }
            } finally {
                synchronized(lock) { client = null }
            }
        }
    }

    private fun accept(): Socket? {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(6)
        while (!stopped) {
            try {
                return server.accept()
            } catch (_: SocketTimeoutException) {
                if (System.nanoTime() - deadline >= 0) throw IOException("Hook stream did not connect")
            }
        }
        return null
    }

    // Keep the bound port reserved until the target releases its stream lease.
    fun stopReading() {
        synchronized(lock) {
            stopped = true
            runCatching { client?.close() }
        }
    }

    override fun close() {
        stopReading()
        server.close()
    }
}

private fun DataInputStream.discard(size: Int) {
    var remaining = size
    while (remaining > 0) {
        val skipped = skipBytes(remaining)
        if (skipped > 0) {
            remaining -= skipped
        } else {
            if (read() == -1) throw EOFException("Truncated hook event frame")
            remaining--
        }
    }
}
