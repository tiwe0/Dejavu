package android.net

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.InputStream

class LocalSocketAddress(val name: String, val namespace: Namespace) { enum class Namespace { ABSTRACT } }
data class Credentials(val uid: Int)
class LocalSocket {
    companion object {
        var response = ""
        var uid = 0
        var onConnect: ((LocalSocketAddress) -> Unit)? = null
        var onRead: ((LocalSocket) -> Unit)? = null
        val requests = mutableListOf<ByteArrayOutputStream>()
    }
    @Volatile var closed = false
    val peerCredentials get() = Credentials(uid)
    var soTimeout = 0
    private val bytes = ByteArrayInputStream(response.toByteArray())
    val inputStream: InputStream = object : InputStream() {
        override fun read(): Int {
            onRead?.invoke(this@LocalSocket)
            if (closed) throw IOException("closed")
            return bytes.read()
        }
    }
    val outputStream = ByteArrayOutputStream().also { requests.add(it) }
    fun connect(address: LocalSocketAddress) { onConnect?.invoke(address) }
    fun shutdownInput() { closed = true }
    fun shutdownOutput() { closed = true }
    fun close() { closed = true }
}
