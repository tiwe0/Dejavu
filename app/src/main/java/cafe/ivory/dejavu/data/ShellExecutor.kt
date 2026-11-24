package cafe.ivory.dejavu.data

import java.io.InputStream
import java.io.OutputStream
import java.util.concurrent.CancellationException

data class ShellResult(val exitCode: Int, val output: String)

/** Cancels the current read-only RPC resource and rejects subsequent stages. */
class RpcCancellation {
    private val lock = Any()
    private var cancelled = false
    private var shutdown: (() -> Unit)? = null

    fun cancel() {
        val action = synchronized(lock) {
            if (cancelled) return
            cancelled = true
            shutdown.also { shutdown = null }
        }
        action?.let { runCatching(it) }
    }

    fun throwIfCancelled() {
        if (synchronized(lock) { cancelled }) throw CancellationException("RPC status request was cancelled")
    }

    fun <T> withResource(close: () -> Unit, action: () -> T): T {
        val alreadyCancelled = synchronized(lock) {
            if (cancelled) true else {
                check(shutdown == null) { "RPC resources must be used sequentially" }
                shutdown = close
                false
            }
        }
        if (alreadyCancelled) {
            runCatching(close)
            throwIfCancelled()
        }
        try {
            throwIfCancelled()
            val result = action()
            throwIfCancelled()
            return result
        } catch (error: Exception) {
            throwIfCancelled()
            throw error
        } finally {
            synchronized(lock) { if (shutdown === close) shutdown = null }
        }
    }
}

/** Minimal root-shell boundary so data and ViewModel layers can be tested on the JVM. */
interface ShellExecutor {
    fun quote(value: String): String = "'" + value.replace("'", "'\\''") + "'"
    fun start(command: String): Process
    fun run(command: String, timeoutSeconds: Long = 20): ShellResult =
        run(command, ByteArray(0), timeoutSeconds)
    fun run(command: String, input: ByteArray, timeoutSeconds: Long = 20): ShellResult
    fun runCancellable(command: String, cancellation: RpcCancellation, timeoutSeconds: Long = 20): ShellResult =
        run(command, timeoutSeconds)

    fun <T> exchangeBinary(
        command: String,
        timeoutMillis: Long,
        cancellation: RpcCancellation?,
        exchange: (InputStream, OutputStream) -> T,
    ): T {
        throw UnsupportedOperationException("exchangeBinary is not implemented")
    }

    fun startLogcat(arguments: String): Process = start("logcat $arguments")
    fun stopLogcat(process: Process?) {
        runCatching { process?.outputStream?.close() }
    }

    fun getProp(name: String): String {
        val result = run("getprop ${quote(name)}")
        return if (result.exitCode == 0) result.output.trim().lineSequence().firstOrNull().orEmpty()
        else ""
    }

    fun readFile(path: String): String? {
        val result = run("cat ${quote(path)}")
        return if (result.exitCode == 0) result.output else null
    }

    fun isRoot(): Boolean = run("id").output.contains("uid=0")
}
