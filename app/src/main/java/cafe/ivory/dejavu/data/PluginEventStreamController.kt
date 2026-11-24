package cafe.ivory.dejavu.data

import android.os.SystemClock
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.security.SecureRandom
import java.time.Instant
import java.util.concurrent.ConcurrentHashMap

/**
 * Owns live Socket / Logcat plugin event streaming with bounded in-memory buffers,
 * 30s deduplication, off-main-thread socket shutdown and cancellable RPC attachment.
 */
internal class PluginEventStreamController(
    private val scope: CoroutineScope,
    private val rpc: DejavuRpcClient = DejavuRpcClient(),
    private val shell: ShellExecutor = RootShell,
    private val formatError: (String) -> String = { it },
    private val onStateUpdate: ((PackageEventsUi?) -> PackageEventsUi?) -> Unit,
) {
    private val lock = Any()
    private var job: Job? = null
    private var process: Process? = null
    private var socket: PluginHookEventSocket? = null
    private var attachCancellation: RpcCancellation? = null
    private val leaseLocks = ConcurrentHashMap<String, Mutex>()

    @Volatile
    private var serial = 0

    fun open(
        row: DirectoryPluginUi,
        target: String,
        listenPort: Int? = null,
    ) {
        val spec = row.plugin.manifest.events ?: return
        val session = row.session ?: return
        if (row.runState != DirectoryPluginState.RUNNING) return
        val id = row.plugin.manifest.id
        close()
        val runSerial = serial
        val openedAt = Instant.now()
        val since = "${openedAt.epochSecond}.${openedAt.nano.toString().padStart(9, '0')}"
        val decoder = if (spec.usesSocket) null else PluginEventDecoder(spec.marker, session)
        val limit = row.plugin.page?.eventLimit ?: 50
        onStateUpdate { PackageEventsUi(id, row.plugin.manifest.name, target, session = session) }
        job = scope.launch(Dispatchers.IO) {
            var localProcess: Process? = null
            var expiryJob: Job? = null
            val seen = linkedMapOf<String, Long>()
            fun deliver(payload: String) {
                if (runSerial != serial) return
                val event = try { JSONObject(payload) } catch (_: Exception) { return }
                val projected = projectPluginEvent(event, spec, row.plugin.manifest.name)
                val now = SystemClock.elapsedRealtime()
                seen.entries.removeAll { now - it.value > 30_000 }
                val key = projected.dedupKey
                if (key != null && seen.put(key, now) != null) return
                if (seen.size > 4096) seen.remove(seen.keys.first())
                val item = projected.item.copy(receivedAtMillis = System.currentTimeMillis())
                updateEvents(runSerial) {
                    it.copy(events = (it.events + item).takeLast(limit))
                }
            }
            try {
                if (decoder == null) {
                    leaseLocks.getOrPut(target) { Mutex() }.withLock {
                        if (runSerial == serial) {
                            readSocket(target, id, session, requireNotNull(spec.topic), listenPort ?: 0, runSerial, ::deliver)
                        }
                    }
                } else {
                    val pid = rpc.processPid(target)
                    if (runSerial != serial) return@launch
                    localProcess = shell.startLogcat(
                        "--pid $pid -v raw -T $since -s ${shell.quote(spec.tag + ":I")} '*:S'",
                    )
                    synchronized(lock) {
                        if (runSerial != serial) return@launch
                        process = localProcess
                    }
                    updateEvents(runSerial) { it.copy(connected = true) }
                    expiryJob = launch(Dispatchers.Default) {
                        while (isActive) {
                            delay(1000)
                            decoder.feed("")
                        }
                    }
                    localProcess.inputStream.bufferedReader().useLines { lines ->
                        lines.forEach { line ->
                            if (!isActive || runSerial != serial) return@launch
                            decoder.feed(line)?.let(::deliver)
                        }
                    }
                }
                if (runSerial == serial) error("Live event stream closed")
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                updateEvents(runSerial) {
                    it.copy(connected = false, error = formatError(error.message.orEmpty()))
                }
            } finally {
                expiryJob?.cancel()
                shell.stopLogcat(localProcess)
                decoder?.clear()
                seen.clear()
                synchronized(lock) {
                    if (process === localProcess) process = null
                }
            }
        }
    }

    private suspend fun readSocket(
        target: String,
        id: String,
        session: String,
        topic: String,
        port: Int,
        runSerial: Int,
        onPayload: (String) -> Unit,
    ) {
        val owner = newToken()
        val eventSocket = PluginHookEventSocket(port)
        val cancellation = RpcCancellation()
        var attaching = false
        try {
            synchronized(lock) {
                if (runSerial != serial) return
                socket = eventSocket
                attachCancellation = cancellation
            }
            updateEvents(runSerial) { it.copy(listenPort = eventSocket.listenPort) }
            currentCoroutineContext().ensureActive()
            attaching = true
            rpc.execute(target, LuaPackageScripts.attachSocket(id, session, owner, eventSocket.listenPort), cancellation)
            currentCoroutineContext().ensureActive()
            eventSocket.readEvents("$topic.$session", {
                updateEvents(runSerial) { it.copy(connected = true) }
            }, onPayload)
        } finally {
            eventSocket.stopReading()
            try {
                if (attaching) {
                    withContext(NonCancellable) {
                        runCatching {
                            rpc.execute(
                                target,
                                LuaPackageScripts.detachSocket(id, session, owner),
                                timeoutMs = DejavuRpcClient.DETACH_TIMEOUT_MS,
                            )
                        }
                    }
                }
            } finally {
                runCatching { eventSocket.close() }
                synchronized(lock) {
                    if (socket === eventSocket) socket = null
                    if (attachCancellation === cancellation) attachCancellation = null
                }
            }
        }
    }

    private fun updateEvents(expectedSerial: Int, update: (PackageEventsUi) -> PackageEventsUi) {
        onStateUpdate { current ->
            if (expectedSerial == serial) current?.let(update) else current
        }
    }

    fun close() {
        val (activeSocket, activeProcess, activeCancel) = synchronized(lock) {
            ++serial
            Triple(
                socket.also { socket = null },
                process.also { process = null },
                attachCancellation.also { attachCancellation = null },
            )
        }
        job?.cancel()
        job = null
        // Never perform socket/process close syscalls on the UI thread.
        if (activeSocket != null || activeProcess != null || activeCancel != null) {
            Thread({
                runCatching { activeCancel?.cancel() }
                runCatching { activeSocket?.stopReading() }
                runCatching { shell.stopLogcat(activeProcess) }
            }, "dejavu-event-stream-stop").apply { isDaemon = true; start() }
        }
        onStateUpdate { null }
    }

    companion object {
        fun newToken(): String = ByteArray(8).also { SecureRandom().nextBytes(it) }
            .joinToString("") { "%02x".format(it.toInt() and 0xff) }
    }
}
