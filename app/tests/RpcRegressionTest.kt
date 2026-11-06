package cafe.ivory.dejavu.data

import java.io.File
import java.io.PipedInputStream
import java.io.PipedOutputStream
import java.util.concurrent.TimeUnit
import java.util.concurrent.CountDownLatch
import java.util.concurrent.atomic.AtomicInteger
import java.net.SocketTimeoutException

fun runRpcRegressionTests() {
    val value = (0..31).map(Int::toChar).joinToString("") + "\u007f0123'\"\\中文🌙"
    val encoded = DejavuRpc.luaString(value)
    check(encoded.none { it.code < 32 || it.code == 127 }) { "Lua literal contains a raw control byte" }
    check("\\000" in encoded && "\\010" in encoded && "\\013" in encoded && "\\1270123" in encoded)
    val lua = File(System.getenv("DEJAVU_TEST_LUA") ?: "zygisk/out/host-tests/lua")
    check(lua.canExecute()) { "RPC quote round-trip needs the host Lua interpreter" }
    val process = ProcessBuilder(lua.absolutePath, "-").start()
    process.outputStream.bufferedWriter().use { it.write("io.write($encoded)") }
    val result = process.inputStream.readBytes()
    check(process.waitFor(5, TimeUnit.SECONDS) && process.exitValue() == 0) {
        process.errorStream.bufferedReader().readText()
    }
    check(result.contentEquals(value.toByteArray(Charsets.UTF_8))) { "Lua argument round-trip changed bytes" }
    check(selectRpcEndpoint("app", sequenceOf("app.99.rpc", "app.12.rpc"), setOf(12))?.pid == 12)
    check(selectRpcEndpoint("app", sequenceOf("app.99.rpc"), setOf(12)) == null)
    val token = "0123456789abcdef".repeat(4)
    check(decodeRpcToken(token)?.size == 32)
    check(decodeRpcToken(token.uppercase())?.contentEquals(decodeRpcToken(token)!!) == true)
    for (bad in listOf("", token.dropLast(1), token + "0", "gg" + token, token + "gg", "0".repeat(64), "  " + token)) {
        check(decodeRpcToken(bad) == null) { "Accepted invalid token: $bad" }
    }
    check(validRpcSocketName("dejavu-123.abc_DEF"))
    for (bad in listOf("", "a".repeat(64), "socket/name", "socket\u0000", "socket\n", "socket:name")) {
        check(!validRpcSocketName(bad))
    }
    val closes = AtomicInteger()
    check(withRpcDeadline(30, { closes.incrementAndGet() }) { "done" } == "done")
    // A canceled timer must never shut down a completed request's socket.
    check(!CountDownLatch(1).await(80, TimeUnit.MILLISECONDS))
    check(closes.get() == 0)
    val unblocked = CountDownLatch(1)
    val started = System.nanoTime()
    val stalled = runCatching {
        withRpcDeadline(30, { closes.incrementAndGet(); unblocked.countDown() }) {
            check(unblocked.await(2, TimeUnit.SECONDS)) { "Deadline did not interrupt blocked I/O" }
            "late result"
        }
    }.exceptionOrNull()
    check(stalled is SocketTimeoutException)
    check(TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - started) < 1500)
    check(closes.get() == 1)
    val failure = IllegalStateException("write failed")
    check(runCatching { withRpcDeadline(30, { closes.incrementAndGet() }) { throw failure } }.exceptionOrNull() === failure)
    check(!CountDownLatch(1).await(80, TimeUnit.MILLISECONDS))
    check(closes.get() == 1)
    PipedInputStream().use { input ->
        PipedOutputStream(input).use { output ->
            val blockedRead = runCatching {
                withRpcDeadline(30, { output.close() }) { input.read() }
            }.exceptionOrNull()
            check(blockedRead is SocketTimeoutException) { "Blocked stream read escaped the deadline" }
        }
    }
    // Occupy the one scheduler thread so the second deadline must use its monotonic clock.
    val timerOccupied = CountDownLatch(1)
    val releaseTimer = CountDownLatch(1)
    val releaseAction = CountDownLatch(1)
    val busy = Thread {
        runCatching {
            withRpcDeadline(10, {
                timerOccupied.countDown()
                releaseTimer.await(2, TimeUnit.SECONDS)
            }) { releaseAction.await(2, TimeUnit.SECONDS) }
        }
    }.apply { start() }
    try {
        check(timerOccupied.await(2, TimeUnit.SECONDS))
        val delayed = runCatching {
            withRpcDeadline(20, { error("timer should still be occupied") }) {
                CountDownLatch(1).await(60, TimeUnit.MILLISECONDS)
            }
        }.exceptionOrNull()
        check(delayed is SocketTimeoutException) { "A delayed scheduler accepted an expired result" }
    } finally {
        releaseTimer.countDown()
        releaseAction.countDown()
        busy.join(2000)
    }
    check(!busy.isAlive)
    println("OK: RPC Lua arguments round-trip all ASCII controls, digits, quotes and Unicode")
    println("OK: RPC live endpoints, strict tokens/socket names, stalled I/O deadline and timer cancellation")
}
