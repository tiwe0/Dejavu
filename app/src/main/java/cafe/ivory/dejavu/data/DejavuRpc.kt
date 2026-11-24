package cafe.ivory.dejavu.data

import android.net.LocalSocket
import android.net.LocalSocketAddress
import org.json.JSONObject
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.EOFException
import java.nio.charset.StandardCharsets
import java.security.SecureRandom

open class RpcException(message: String) : Exception(message)

internal class TargetProcessNotRunning(val process: String) :
    RpcException("target process is not running: $process")

private sealed interface RpcEndpointTransport {
    data object Bridge : RpcEndpointTransport
    data class Direct(val socket: String, val token: ByteArray) : RpcEndpointTransport
}

private data class RpcMetadata(val pid: Int, val transport: RpcEndpointTransport)

/** Injectable client for Dejavu's Lua control channel. */
class DejavuRpcClient(private val shell: ShellExecutor = RootShell) {
    fun execute(process: String, script: String, timeoutMs: Int = EXCHANGE_TIMEOUT_MS): String =
        executeRequest(process, script, null, timeoutMs)

    internal fun execute(
        process: String,
        script: String,
        cancellation: RpcCancellation,
        timeoutMs: Int = EXCHANGE_TIMEOUT_MS,
    ): String = executeRequest(process, script, cancellation, timeoutMs)

    fun processPid(process: String): Int = readMetadata(process).pid

    fun unloadPlugin(process: String, name: String): String =
        execute(process, "return plugin.unload(${LuaPackageScripts.quote(name)})")

    fun pluginSkill(process: String, name: String): String =
        execute(process, "return plugin.skill(${LuaPackageScripts.quote(name)})")

    fun loadPlugin(process: String, path: String): String {
        val function = if (path.endsWith(".c")) "load_jit" else "load"
        return execute(process, "return plugin.$function(${LuaPackageScripts.quote(path)})")
    }

    /** Lists distinct process names that currently have a live RPC metadata file under run/. */
    fun listActiveRpcProcesses(): List<String> {
        val listing = shell.run("ls -1 ${shell.quote("$MODULE_DIR/run")}")
        check(listing.exitCode == 0) { "Cannot inspect running plugin processes: ${listing.output.trim().take(160)}" }
        val endpointName = Regex("^([A-Za-z0-9_.:-]{1,128})\\.[1-9][0-9]*\\.rpc$")
        return listing.output.lineSequence().mapNotNull {
            endpointName.matchEntire(it.trim())?.groupValues?.get(1)
        }.distinct().toList()
    }

    private fun executeRequest(
        process: String,
        script: String,
        cancellation: RpcCancellation?,
        timeoutMs: Int,
    ): String {
        val payload = script.toByteArray(StandardCharsets.UTF_8)
        if (payload.size > MAX_PAYLOAD) throw RpcException("script is too large")
        val metadata = readMetadata(process, cancellation)
        return when (val transport = metadata.transport) {
            RpcEndpointTransport.Bridge -> shell.exchangeBinary(
                "exec ${shell.quote(BRIDGE)} ${shell.quote(process)} ${metadata.pid}",
                timeoutMs.toLong(),
                cancellation,
            ) { input, output ->
                exchange(DataInputStream(input), DataOutputStream(BufferedOutputStream(output)), payload)
            }
            is RpcEndpointTransport.Direct -> executeDirect(transport, payload, cancellation, timeoutMs)
        }
    }

    private fun executeDirect(
        endpoint: RpcEndpointTransport.Direct,
        payload: ByteArray,
        cancellation: RpcCancellation?,
        timeoutMs: Int,
    ): String {
        val socket = LocalSocket()
        val shutdown = {
            // shutdown wakes blocked reads/writes; close alone need not interrupt a syscall.
            runCatching { socket.shutdownInput() }
            runCatching { socket.shutdownOutput() }
            runCatching { socket.close() }
            Unit
        }
        val runExchange = {
            // Accessing the streams creates the descriptor; setSoTimeout itself does not.
            val input = DataInputStream(BufferedInputStream(socket.inputStream))
            val output = DataOutputStream(BufferedOutputStream(socket.outputStream))
            // LocalSocket sets both SO_RCVTIMEO and SO_SNDTIMEO. Set it before connect.
            socket.soTimeout = timeoutMs
            withRpcDeadline(timeoutMs.toLong(), shutdown) {
                socket.connect(
                    LocalSocketAddress(endpoint.socket, LocalSocketAddress.Namespace.ABSTRACT),
                )
                if (socket.peerCredentials.uid != 0) {
                    throw RpcException("RPC server is not the root companion")
                }
                writeFrame(output, endpoint.token)
                val ack = readFrame(input)
                if (ack.isNotEmpty()) {
                    throw RpcException("RPC authentication rejected")
                }
                exchange(input, output, payload)
            }
        }
        try {
            return if (cancellation == null) runExchange() else cancellation.withResource(shutdown, runExchange)
        } finally {
            try {
                socket.close()
            } catch (_: Exception) {
            }
        }
    }

    private fun readMetadata(process: String, cancellation: RpcCancellation? = null): RpcMetadata {
        fun read(command: String): ShellResult =
            if (cancellation == null) shell.run(command) else shell.runCancellable(command, cancellation)
        if (process.startsWith('-') || !process.matches(Regex("[A-Za-z0-9_.:-]{1,128}"))) {
            throw RpcException("invalid target process name")
        }
        val running = read("pidof ${shell.quote(process)}")
        val livePids = running.output.trim().split(Regex("\\s+")).mapNotNull {
            it.toIntOrNull()?.takeIf { pid -> pid > 0 }
        }.toSet()
        if (running.exitCode != 0 || livePids.isEmpty()) {
            throw TargetProcessNotRunning(process)
        }
        val listing = read("ls -1t ${shell.quote("$MODULE_DIR/run")}")
        if (listing.exitCode != 0) {
            throw RpcException("unable to list Dejavu runtime metadata")
        }
        val endpoint = selectRpcEndpoint(process, listing.output.lineSequence().map { it.trim() }, livePids)
            ?: throw RpcException("no RPC endpoint for $process; is the app running with the agent?")
        val cmdline = read("tr '\\000' '\\n' < /proc/${endpoint.pid}/cmdline | head -n 1")
        if (cmdline.exitCode != 0 || cmdline.output.trimEnd('\r', '\n') != process) {
            throw RpcException("RPC endpoint process no longer matches $process")
        }
        val bridgePath = shell.quote(BRIDGE)
        val metadataPath = shell.quote("$MODULE_DIR/run/${endpoint.fileName}")
        val metadata = read(
            """
            if [ -f $bridgePath ] && [ -x $bridgePath ]; then
                printf '%s\n' '$BRIDGE_AVAILABLE'
            elif [ -e $bridgePath ] || [ -L $bridgePath ]; then
                printf '%s\n' '$BRIDGE_UNUSABLE'
            else
                cat $metadataPath
            fi
            """.trimIndent(),
        )
        if (metadata.exitCode != 0) throw RpcException("unable to read RPC metadata")
        val raw = metadata.output
        when (raw.trim()) {
            BRIDGE_AVAILABLE -> return RpcMetadata(endpoint.pid, RpcEndpointTransport.Bridge)
            BRIDGE_UNUSABLE -> throw RpcException("installed RPC bridge is not executable")
        }
        val json = try {
            JSONObject(raw)
        } catch (_: Exception) {
            throw RpcException("invalid RPC metadata")
        }
        if (json.optInt("version") != VERSION) {
            throw RpcException("unsupported RPC metadata version")
        }
        if (json.optInt("pid") != endpoint.pid) {
            throw RpcException("RPC metadata PID does not match its filename")
        }
        val tokenText = json.opt("token") as? String ?: throw RpcException("invalid RPC token")
        val token = decodeRpcToken(tokenText) ?: throw RpcException("invalid RPC token")
        val socketName = json.opt("socket") as? String ?: throw RpcException("invalid RPC socket name")
        if (!validRpcSocketName(socketName)) throw RpcException("invalid RPC socket name")
        return RpcMetadata(
            pid = endpoint.pid,
            transport = RpcEndpointTransport.Direct(socketName, token),
        )
    }

    companion object {
        private const val BRIDGE = "$MODULE_DIR/bin/dejavu-rpc-bridge"
        private const val BRIDGE_AVAILABLE = "dejavu-rpc-bridge"
        private const val BRIDGE_UNUSABLE = "dejavu-rpc-bridge-unusable"
        private const val MAGIC = 0x444A5250
        private const val VERSION = 1
        private const val EXECUTE_REQUEST = 1
        private const val EXECUTE_RESPONSE = 2
        private const val OK = 0
        private const val MAX_PAYLOAD = 1024 * 1024
        private const val HEADER_SIZE = 28
        // Allow the agent's 25s Lua wait and companion's 30s relay deadline to return an error.
        internal const val EXCHANGE_TIMEOUT_MS = 40_000
        internal const val DETACH_TIMEOUT_MS = 5_000

        internal fun exchange(input: DataInputStream, output: DataOutputStream, payload: ByteArray): String {
            val requestId = (SecureRandom().nextLong() and Long.MAX_VALUE).let {
                if (it == 0L) 1L else it
            }
            output.writeIntLE(HEADER_SIZE + payload.size)
            output.writeIntLE(MAGIC)
            output.writeIntLE(VERSION)
            output.writeIntLE(EXECUTE_REQUEST)
            output.writeIntLE(OK)
            output.writeLongLE(requestId)
            output.writeIntLE(payload.size)
            output.write(payload)
            output.flush()

            val frame = readFrame(input)
            if (frame.size < HEADER_SIZE) {
                throw RpcException("truncated RPC response")
            }
            val magic = frame.intLE(0)
            val version = frame.intLE(4)
            val type = frame.intLE(8)
            val status = frame.intLE(12)
            val responseId = frame.longLE(16)
            val payloadSize = frame.intLE(24)
            if (magic != MAGIC || version != VERSION || type != EXECUTE_RESPONSE ||
                responseId != requestId || payloadSize < 0 ||
                payloadSize != frame.size - HEADER_SIZE
            ) {
                throw RpcException("invalid RPC response")
            }
            val response = String(frame, HEADER_SIZE, payloadSize, StandardCharsets.UTF_8)
            if (status != OK) {
                throw RpcException(response.ifBlank { "Lua execution failed" })
            }
            return response
        }

        private fun readFrame(input: DataInputStream): ByteArray {
            try {
                val size = input.readIntLE()
                if (size < 0 || size > MAX_PAYLOAD + HEADER_SIZE) {
                    throw RpcException("RPC frame is too large")
                }
                val bytes = ByteArray(size)
                input.readFully(bytes)
                return bytes
            } catch (_: EOFException) {
                throw RpcException("RPC connection closed unexpectedly")
            }
        }

        private fun writeFrame(output: DataOutputStream, payload: ByteArray) {
            if (payload.size > MAX_PAYLOAD + HEADER_SIZE) {
                throw RpcException("RPC frame is too large")
            }
            output.writeIntLE(payload.size)
            output.write(payload)
            output.flush()
        }

        private fun DataInputStream.readIntLE(): Int {
            val b0 = readUnsignedByte()
            val b1 = readUnsignedByte()
            val b2 = readUnsignedByte()
            val b3 = readUnsignedByte()
            return b0 or (b1 shl 8) or (b2 shl 16) or (b3 shl 24)
        }

        private fun DataOutputStream.writeIntLE(value: Int) {
            writeByte(value and 0xFF)
            writeByte((value ushr 8) and 0xFF)
            writeByte((value ushr 16) and 0xFF)
            writeByte((value ushr 24) and 0xFF)
        }

        private fun DataOutputStream.writeLongLE(value: Long) {
            for (shift in 0 until 64 step 8) {
                writeByte(((value ushr shift) and 0xFF).toInt())
            }
        }

        private fun ByteArray.intLE(offset: Int): Int =
            (this[offset].toInt() and 0xFF) or
                ((this[offset + 1].toInt() and 0xFF) shl 8) or
                ((this[offset + 2].toInt() and 0xFF) shl 16) or
                ((this[offset + 3].toInt() and 0xFF) shl 24)

        private fun ByteArray.longLE(offset: Int): Long {
            var result = 0L
            for (index in 0 until 8) {
                result = result or ((this[offset + index].toLong() and 0xFF) shl (index * 8))
            }
            return result
        }
    }
}

/** Default singleton delegating to [DejavuRpcClient] for existing callers and regression tests. */
object DejavuRpc {
    private val default = DejavuRpcClient()

    fun execute(process: String, script: String): String = default.execute(process, script)

    internal fun execute(process: String, script: String, cancellation: RpcCancellation): String =
        default.execute(process, script, cancellation)

    internal fun exchange(input: DataInputStream, output: DataOutputStream, payload: ByteArray): String =
        DejavuRpcClient.exchange(input, output, payload)

    fun processPid(process: String): Int = default.processPid(process)

    fun luaString(value: String): String = LuaPackageScripts.quote(value)

    fun unloadPlugin(process: String, name: String): String = default.unloadPlugin(process, name)

    fun pluginSkill(process: String, name: String): String = default.pluginSkill(process, name)

    fun loadPlugin(process: String, path: String): String = default.loadPlugin(process, path)
}
