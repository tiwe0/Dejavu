package android.net

import java.io.InputStream
import java.io.OutputStream

class LocalSocketAddress(val name: String, val namespace: Namespace) {
    enum class Namespace { ABSTRACT }
}

/** Host tests cover pure RPC helpers; the Android transport requires a device. */
data class Credentials(val pid: Int, val uid: Int, val gid: Int)

class LocalSocket {
    val peerCredentials: Credentials get() = error("Android socket unavailable in host tests")
    var soTimeout: Int = 0
    val inputStream: InputStream get() = error("Android socket unavailable in host tests")
    val outputStream: OutputStream get() = error("Android socket unavailable in host tests")
    fun connect(address: LocalSocketAddress) { error("Android socket unavailable: ${address.name}") }
    fun shutdownInput() {}
    fun shutdownOutput() {}
    fun close() {}
}
