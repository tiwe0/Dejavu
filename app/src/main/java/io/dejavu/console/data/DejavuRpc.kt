package io.dejavu.console.data

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

class RpcException(message: String) : Exception(message)

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

    fun execute(process: String, script: String): String {
        val metadata = readMetadata(process)
        val socket = LocalSocket()
        try {
            socket.connect(
                LocalSocketAddress(metadata.socket, LocalSocketAddress.Namespace.ABSTRACT),
            )
            socket.soTimeout = 8000
            val input = DataInputStream(BufferedInputStream(socket.inputStream))
            val output = DataOutputStream(BufferedOutputStream(socket.outputStream))
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
            return response
        } finally {
            try {
                socket.close()
            } catch (_: Exception) {
            }
        }
    }

    fun listPluginStatus(process: String): String = execute(process, "return plugin.list()")

    fun luaString(value: String): String =
        "'" + value.replace("\\", "\\\\").replace("'", "\\'") + "'"

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

    fun loadPlugin(process: String, path: String): String =
        execute(process, "return plugin.load(${luaString(path)})")

    private fun readMetadata(process: String): RpcMetadata {
        if (!process.matches(Regex("[A-Za-z0-9_.:-]{1,128}"))) {
            throw RpcException("invalid target process name")
        }
        val listing = RootShell.run("ls -1t ${RootShell.quote("$MODULE_DIR/run")}")
        if (listing.exitCode != 0) {
            throw RpcException("unable to list Dejavu runtime metadata")
        }
        val metadataName = listing.output.lineSequence()
            .map { it.trim() }
            .firstOrNull { it.startsWith("$process.") && it.endsWith(".rpc") }
            ?: throw RpcException("no RPC endpoint for $process; is the app running with the agent?")
        val raw = RootShell.readFile("$MODULE_DIR/run/$metadataName")
            ?: throw RpcException("unable to read RPC metadata")
        val json = try {
            JSONObject(raw)
        } catch (error: Exception) {
            throw RpcException("invalid RPC metadata")
        }
        if (json.optInt("version") != VERSION) {
            throw RpcException("unsupported RPC metadata version")
        }
        val tokenText = json.optString("token")
        val token = tokenText.chunked(2).mapNotNull { it.toIntOrNull(16)?.toByte() }.toByteArray()
        if (token.size != 32) {
            throw RpcException("invalid RPC token")
        }
        return RpcMetadata(
            pid = json.optInt("pid"),
            socket = json.optString("socket"),
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
