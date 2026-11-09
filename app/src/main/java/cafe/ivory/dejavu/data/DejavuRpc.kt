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

private data class RpcMetadata(val pid: Int, val socket: String, val token: ByteArray)

/**
 * Direct client for Dejavu's abstract-socket Lua control channel. Metadata is
 * root-only, so the app uses RootShell to read it; the socket itself is
 * authenticated by the per-process 32-byte token.
 */
object DejavuRpc {
    private const val MODULE_DIR = "/data/adb/modules/dejavu_zygisk"
    private const val MAGIC = 0x444A5250
    private const val VERSION = 1
    private const val EXECUTE_REQUEST = 1
    private const val EXECUTE_RESPONSE = 2
    private const val OK = 0
    private const val MAX_PAYLOAD = 1024 * 1024
    private const val HEADER_SIZE = 28
    // Allow the agent's 25s Lua wait and companion's 30s relay deadline to return an error.
    private const val EXCHANGE_TIMEOUT_MS = 40_000

    fun execute(process: String, script: String): String {
        val metadata = readMetadata(process)
        val socket = LocalSocket()
        try {
            // Accessing the streams creates the descriptor; setSoTimeout itself does not.
            val input = DataInputStream(BufferedInputStream(socket.inputStream))
            val output = DataOutputStream(BufferedOutputStream(socket.outputStream))
            // LocalSocket sets both SO_RCVTIMEO and SO_SNDTIMEO. Set it before connect.
            socket.soTimeout = EXCHANGE_TIMEOUT_MS
            return withRpcDeadline(EXCHANGE_TIMEOUT_MS.toLong(), {
                // shutdown wakes blocked reads/writes; close alone need not interrupt a syscall.
                runCatching { socket.shutdownInput() }
                runCatching { socket.shutdownOutput() }
                runCatching { socket.close() }
            }) {
                socket.connect(
                    LocalSocketAddress(metadata.socket, LocalSocketAddress.Namespace.ABSTRACT),
                )
                if (socket.peerCredentials.uid != 0) {
                    throw RpcException("RPC server is not the root companion")
                }
                writeFrame(output, metadata.token)
                val ack = readFrame(input)
                if (ack.isNotEmpty()) {
                    throw RpcException("RPC authentication rejected")
                }
                val requestId = (SecureRandom().nextLong() and Long.MAX_VALUE).let {
                    if (it == 0L) 1L else it
                }
                val payload = script.toByteArray(StandardCharsets.UTF_8)
                if (payload.size > MAX_PAYLOAD) {
                    throw RpcException("script is too large")
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
                response
            }
        } finally {
            try {
                socket.close()
            } catch (_: Exception) {
            }
        }
    }

    fun listPluginStatus(process: String): String = execute(process, "return plugin.list()")

    fun processPid(process: String): Int = readMetadata(process).pid

    fun luaString(value: String): String = LuaPackageScripts.quote(value)

    fun callPlugin(process: String, plugin: String, function: String, vararg args: String): String {
        val arguments = buildString {
            append(luaString(plugin)).append(", ").append(luaString(function))
            args.forEach { append(", ").append(luaString(it)) }
        }
        return execute(process, "return plugin.call($arguments)")
    }

    fun unloadPlugin(process: String, name: String): String =
        execute(process, "return plugin.unload(${luaString(name)})")

    fun pluginSkill(process: String, name: String): String =
        execute(process, "return plugin.skill(${luaString(name)})")

    fun loadPlugin(process: String, path: String): String {
        val function = if (path.endsWith(".c")) "load_jit" else "load"
        return execute(process, "return plugin.$function(${luaString(path)})")
    }

    private fun readMetadata(process: String): RpcMetadata {
        if (!process.matches(Regex("[A-Za-z0-9_.:-]{1,128}"))) {
            throw RpcException("invalid target process name")
        }
        val running = RootShell.run("pidof ${RootShell.quote(process)}")
        val livePids = running.output.trim().split(Regex("\\s+")).mapNotNull {
            it.toIntOrNull()?.takeIf { pid -> pid > 0 }
        }.toSet()
        if (running.exitCode != 0 || livePids.isEmpty()) {
            throw TargetProcessNotRunning(process)
        }
        val listing = RootShell.run("ls -1t ${RootShell.quote("$MODULE_DIR/run")}")
        if (listing.exitCode != 0) {
            throw RpcException("unable to list Dejavu runtime metadata")
        }
        val endpoint = selectRpcEndpoint(process, listing.output.lineSequence().map { it.trim() }, livePids)
            ?: throw RpcException("no RPC endpoint for $process; is the app running with the agent?")
        val cmdline = RootShell.run("tr '\\000' '\\n' < /proc/${endpoint.pid}/cmdline | head -n 1")
        if (cmdline.exitCode != 0 || cmdline.output.trimEnd('\r', '\n') != process) {
            throw RpcException("RPC endpoint process no longer matches $process")
        }
        val raw = RootShell.readFile("$MODULE_DIR/run/${endpoint.fileName}")
            ?: throw RpcException("unable to read RPC metadata")
        val json = try {
            JSONObject(raw)
        } catch (error: Exception) {
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
            socket = socketName,
            token = token,
        )
    }

    private fun readFrame(input: DataInputStream): ByteArray {
        val size = input.readIntLE()
        if (size < 0 || size > MAX_PAYLOAD + HEADER_SIZE) {
            throw RpcException("RPC frame is too large")
        }
        val bytes = ByteArray(size)
        try {
            input.readFully(bytes)
        } catch (error: EOFException) {
            throw RpcException("RPC connection closed unexpectedly")
        }
        return bytes
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
