package cafe.ivory.dejavu.data

import java.io.DataOutputStream
import java.io.File
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import kotlin.concurrent.thread

fun runPluginHookEventSocketTests() {
    val topic = "test.message.0123456789abcdef"
    val body = """{"title":"合成事件","content":"你好 🧪"}"""
    PluginHookEventSocket(0).use { listener ->
        Socket("127.0.0.1", listener.listenPort).use { peer ->
            val sender = thread(isDaemon = true) {
                val out = DataOutputStream(peer.getOutputStream())
                out.writeEvent("test.message.ffffffffffffffff", body.toByteArray())
                out.writeEvent("x".repeat(129), body.toByteArray())
                out.writeEvent(topic, ByteArray(70_000))
                out.writeEvent(topic, byteArrayOf(0xc3.toByte(), 0x28))
                out.writeEvent(topic, body.toByteArray())
                peer.shutdownOutput()
            }
            val received = mutableListOf<String>()
            listener.readEvents(topic, {}, received::add)
            sender.join(2000)
            check(!sender.isAlive)
            check(received == listOf(body))
        }
    }
    for (size in listOf(-1, 2, Int.MAX_VALUE)) {
        PluginHookEventSocket(0).use { listener ->
            Socket("127.0.0.1", listener.listenPort).use { peer ->
                DataOutputStream(peer.getOutputStream()).writeInt(size)
                peer.shutdownOutput()
                check(runCatching { listener.readEvents(topic, {}, {}) }.isFailure)
            }
        }
    }
    PluginHookEventSocket(0).use { listener ->
        Socket("127.0.0.1", listener.listenPort).use {
            val accepted = CountDownLatch(1)
            val unexpected = AtomicReference<Throwable>()
            val reader = thread(isDaemon = true) {
                try {
                    listener.readEvents(topic, { accepted.countDown() }) { error("Unexpected event") }
                } catch (_: java.net.SocketException) {
                    // stopReading can interrupt a blocked socket read.
                } catch (error: Throwable) {
                    unexpected.set(error)
                }
            }
            check(accepted.await(2, TimeUnit.SECONDS))
            listener.stopReading()
            reader.join(2000)
            check(!reader.isAlive && unexpected.get() == null)
            ServerSocket().use { other ->
                check(runCatching { other.bind(InetSocketAddress("127.0.0.1", listener.listenPort)) }.isFailure)
            }
        }
    }
    println("OK: loopback framing, UTF-8, session filtering, and listener cancellation")
    runPackageSocketLeaseTests()
}

private fun DataOutputStream.writeEvent(topic: String, data: ByteArray) {
    val name = topic.toByteArray()
    writeInt(2 + name.size + data.size)
    writeShort(name.size)
    write(name)
    write(data)
}

private fun runPackageSocketLeaseTests() {
    val lua = System.getenv("DEJAVU_TEST_LUA") ?: "zygisk/out/host-tests/lua"
    if (!File(lua).canExecute()) {
        println("SKIP: plugin socket leases require DEJAVU_TEST_LUA or make -C zygisk test")
        return
    }
    val id = "socket-test"
    val session = "0123456789abcdef"
    val first = "1111111111111111"
    val second = "2222222222222222"
    val pkg = ImportedPluginPackage(
        PluginPackageManifest(
            id, "Socket test", "1", entry = "app.lua", files = listOf("app.lua", "ui.json"),
            ui = "ui.json", events = PluginEventSpec(format = "hook-socket-json-v1", topic = "test.message"),
        ),
        mapOf(
            "app.lua" to """
                return {
                    start = function(ctx) assert(not pcall(ctx.read, "ui.json")) end,
                    status = function() return "running" end,
                    stop = function() stopped = stopped + 1 end,
                    actions = {sample = function(ctx) assert(ctx.session == "$session"); called = called + 1 end}
                }
            """.trimIndent(),
            "ui.json" to """{"version":1,"title":"Test","blocks":[{"type":"events","limit":50}]}""",
        ),
    )
    fun run(script: String) = "run([==[\n$script]==])"
    val script = """
        local function run(source) return assert(load(source))() end
        local live, next_lease, released = nil, 0, 0
        called, stopped = 0, 0
        hook = {
            stream_acquire = function(endpoint)
                assert(endpoint == "127.0.0.1:39001" or endpoint == "127.0.0.1:39002")
                assert(live == nil, "event channel already owned")
                next_lease = next_lease + 1
                live = next_lease
                return live
            end,
            stream_release = function(lease)
                if live ~= lease then return false end
                live, released = nil, released + 1
                return true
            end
        }
        ${run(LuaPackageScripts.start(pkg, session))}
        live = 100
        assert(not pcall(function() ${run(LuaPackageScripts.attachSocket(id, session, first, 39001))} end))
        assert(live == 100, "foreign lease was stopped")
        live = nil
        ${run(LuaPackageScripts.attachSocket(id, session, first, 39001))}
        ${run(LuaPackageScripts.attachSocket(id, session, first, 39001))}
        assert(next_lease == 1 and released == 0)
        ${run(LuaPackageScripts.attachSocket(id, session, second, 39002))}
        ${run(LuaPackageScripts.detachSocket(id, session, first))}
        assert(live == 2 and released == 1, "stale viewer detached the replacement")
        ${run(LuaPackageScripts.runAction(id, session, "sample"))}
        assert(called == 1)
        assert(not pcall(function() ${run(LuaPackageScripts.runAction(id, first, "sample"))} end))
        ${run(LuaPackageScripts.stop(id, session))}
        ${run(LuaPackageScripts.detachSocket(id, session, second))}
        assert(stopped == 1 and live == nil and released == 2)
    """.trimIndent()
    val file = File.createTempFile("dejavu-package-socket-", ".lua")
    try {
        file.writeText(script)
        val process = ProcessBuilder(lua, file.absolutePath).redirectErrorStream(true).start()
        check(process.waitFor(10, TimeUnit.SECONDS)) {
            process.destroyForcibly()
            "Lua socket lease test timed out"
        }
        check(process.exitValue() == 0) { process.inputStream.bufferedReader().readText() }
        println("OK: Lua socket lease ownership, session-bound action, and stop cleanup")
    } finally {
        file.delete()
    }
}
