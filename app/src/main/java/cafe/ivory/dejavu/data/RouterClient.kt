package cafe.ivory.dejavu.data

import android.net.LocalSocket
import android.net.LocalSocketAddress
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.util.concurrent.atomic.AtomicLong
import java.util.concurrent.atomic.AtomicReference

internal data class RouterRoute(
    val route: String, val app: String = "", val online: Boolean = false,
    val configured: Boolean = false, val userId: Int = 0, val packageName: String = "",
    val activity: String = "", val pluginPath: String = "",
)
internal data class RouterSnapshot(
    val pid: Int, val hubConnected: Boolean, val pluginCount: Int, val pendingJobs: Int,
    val listenPort: Int, val hubHost: String, val hubPort: Int, val deviceId: String,
    val tokenConfigured: Boolean, val routes: List<RouterRoute>,
    val pluginSocket: String = "", val listenTcp: Boolean = false,
)
internal data class RouterService(val installed: Boolean = false, val running: Boolean = false, val enabled: Boolean = false)
internal class RouterEndpoint(val name: String, val token: String) {
    override fun toString(): String = "RouterEndpoint(name=$name)"
    companion object {
        fun parse(text: String): RouterEndpoint {
            require(text.length <= 4096)
            val json = JSONObject(text)
            require(json.getString("transport") == "unix" && json.getString("namespace") == "abstract")
            val name = json.getString("name")
            val token = json.getString("token")
            require(name.matches(Regex("[A-Za-z0-9_.-]{1,100}")) && token.matches(Regex("[a-fA-F0-9]{64}")))
            return RouterEndpoint(name, token)
        }
    }
}
internal object RouterProtocol {
    const val MAX_RESPONSE = 1024 * 1024
    fun snapshot(response: JSONObject): RouterSnapshot {
        if (!response.optBoolean("ok")) throw IOException("Router operation rejected")
        val status = response.getJSONObject("status")
        val routes = status.getJSONArray("routes")
        require(routes.length() <= 1024)
        return RouterSnapshot(
            status.getInt("pid"), status.getBoolean("hub_connected"), status.getInt("plugin_count"),
            status.getInt("pending_jobs"), status.getInt("listen_port"), status.getString("hub_host"),
            status.getInt("hub_port"), status.getString("device_id"), status.getBoolean("token_configured"),
            List(routes.length()) { index ->
                val row = routes.getJSONObject(index)
                RouterRoute(row.getString("route"), row.optString("app"), row.optBoolean("online"),
                    row.optBoolean("configured"), row.optInt("user_id"), row.optString("package"),
                    row.optString("activity"), row.optString("plugin_path"))
            },
            status.optString("plugin_socket"), status.optBoolean("listen_tcp"),
        )
    }
}

/** Root is used only for lifecycle and discovery; live snapshots use the authenticated socket. */
internal class RouterClient(
    private val shell: (String) -> ShellResult,
    private val readShell: (String, RpcCancellation) -> ShellResult,
) {
    constructor() : this(
        { RootShell.run(it, timeoutSeconds = 12) },
        { command, cancellation -> RootShell.runCancellable(command, cancellation, timeoutSeconds = 12) },
    )
    constructor(shell: (String) -> ShellResult) : this(shell, { command, cancellation ->
        cancellation.throwIfCancelled()
        shell(command).also { cancellation.throwIfCancelled() }
    })
    private val socket = AtomicReference<LocalSocket?>()
    private val generation = AtomicLong()
    private val rootRead = AtomicReference<RpcCancellation?>()
    @Volatile private var endpoint: RouterEndpoint? = null
    private val control = "/data/adb/modules/dejavu_zygisk/router-control.sh"

    fun cancelRead() {
        generation.incrementAndGet()
        rootRead.getAndSet(null)?.cancel()
        socket.getAndSet(null)?.let(::closeSocket)
    }
    private fun readRoot(command: String, expected: Long = generation.get()): ShellResult {
        val cancellation = RpcCancellation()
        check(rootRead.compareAndSet(null, cancellation)) { "Router root reads must be sequential" }
        try {
            if (generation.get() != expected) cancellation.cancel()
            cancellation.throwIfCancelled()
            return readShell(command, cancellation).also { cancellation.throwIfCancelled() }
        } finally {
            rootRead.compareAndSet(cancellation, null)
        }
    }
    private fun closeSocket(connection: LocalSocket) {
        runCatching { connection.shutdownInput() }
        runCatching { connection.shutdownOutput() }
        runCatching { connection.close() }
    }
    fun forgetEndpoint() { endpoint = null; cancelRead() }
    fun lifecycle(operation: String): RouterService {
        require(operation in setOf("status", "start", "stop"))
        val command = "if [ -f '$control' ]; then sh '$control' $operation; else printf '%s\\n' '{\"installed\":false,\"running\":false,\"enabled\":false}'; fi"
        val result = if (operation == "status") readRoot(command) else shell(command)
        if (result.exitCode != 0) throw IOException("Router lifecycle operation failed")
        val value = JSONObject(result.output.trim())
        val service = RouterService(value.optBoolean("installed"), value.optBoolean("running"), value.optBoolean("enabled"))
        if (!service.running || operation != "status") forgetEndpoint()
        return service
    }
    fun request(operation: String, arguments: JSONObject = JSONObject()): RouterSnapshot {
        val expected = generation.get()
        val auth = endpoint ?: run {
            val result = readRoot("cat /data/adb/dejavu/router/control.json", expected)
            if (result.exitCode != 0) throw IOException("Router management endpoint unavailable")
            RouterEndpoint.parse(result.output.trim()).also { endpoint = it }
        }
        if (generation.get() != expected) throw IOException("Router request cancelled")
        val request = JSONObject(arguments.toString()).put("op", operation).put("token", auth.token)
        val bytes = (request.toString() + "\n").toByteArray(Charsets.UTF_8)
        require(bytes.size <= 32 * 1024)
        val connection = LocalSocket()
        check(socket.compareAndSet(null, connection)) { "Router requests must be sequential" }
        try {
            if (generation.get() != expected) throw IOException("Router request cancelled")
            val input = connection.inputStream.buffered()
            val outputStream = connection.outputStream
            connection.soTimeout = 2500
            return withRpcDeadline(4000, { closeSocket(connection) }) {
                connection.connect(LocalSocketAddress(auth.name, LocalSocketAddress.Namespace.ABSTRACT))
                if (connection.peerCredentials.uid != 0) throw IOException("Router peer is not root")
                outputStream.write(bytes)
                val output = ByteArrayOutputStream()
                while (true) {
                    val next = input.read()
                    if (next < 0) throw IOException("Incomplete router response")
                    if (next == 10) break
                    if (output.size() >= RouterProtocol.MAX_RESPONSE) throw IOException("Router response exceeds limit")
                    output.write(next)
                }
                RouterProtocol.snapshot(JSONObject(output.toString(Charsets.UTF_8.name())))
            }
        } finally {
            socket.compareAndSet(connection, null)
            closeSocket(connection)
        }
    }
}
