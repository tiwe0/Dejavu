package cafe.ivory.dejavu.data

import java.net.SocketTimeoutException
import java.util.concurrent.ScheduledThreadPoolExecutor
import java.util.concurrent.TimeUnit

private val rpcDeadlineScheduler = ScheduledThreadPoolExecutor(1) { runnable ->
    Thread(runnable, "dejavu-rpc-deadline").apply { isDaemon = true }
}.apply {
    removeOnCancelPolicy = true
    setKeepAliveTime(1, TimeUnit.SECONDS)
    allowCoreThreadTimeOut(true)
}

/** Shut down blocked socket operations at one overall deadline, even if bytes keep arriving. */
internal fun <T> withRpcDeadline(timeoutMillis: Long, shutdown: () -> Unit, action: () -> T): T {
    val started = System.nanoTime()
    val budgetNanos = TimeUnit.MILLISECONDS.toNanos(timeoutMillis)
    val lock = Any()
    var finished = false
    var expired = false
    val timer = rpcDeadlineScheduler.schedule({
        synchronized(lock) {
            if (!finished) {
                expired = true
                shutdown()
            }
        }
    }, timeoutMillis, TimeUnit.MILLISECONDS)
    try {
        val result = action()
        synchronized(lock) {
            if (expired || System.nanoTime() - started >= budgetNanos) {
                throw SocketTimeoutException("RPC deadline exceeded; execution outcome may be unknown")
            }
            finished = true
        }
        return result
    } catch (error: Exception) {
        if (synchronized(lock) { expired } && error !is SocketTimeoutException) {
            throw SocketTimeoutException("RPC deadline exceeded; execution outcome may be unknown").apply {
                initCause(error)
            }
        }
        throw error
    } finally {
        synchronized(lock) { finished = true }
        // Remove canceled tasks immediately so they do not retain sockets for the full timeout.
        timer.cancel(false)
    }
}
