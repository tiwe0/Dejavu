package cafe.ivory.dejavu.data

import java.io.File
import java.io.PipedInputStream
import java.io.PipedOutputStream
import java.util.concurrent.TimeUnit
import java.util.concurrent.CountDownLatch
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import java.util.concurrent.CancellationException
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
    runRpcCancellationRegressionTests()
    println("OK: RPC Lua arguments round-trip all ASCII controls, digits, quotes and Unicode")
    println("OK: RPC live endpoints, strict tokens/socket names, stalled I/O deadline and timer cancellation")
}

private fun runRpcCancellationRegressionTests() {
    val completed = RpcCancellation()
    val finishedCloses = AtomicInteger()
    check(completed.withResource({ finishedCloses.incrementAndGet() }) { "done" } == "done")
    completed.cancel()
    check(finishedCloses.get() == 0) { "Cancellation touched an already completed resource" }
    val subsequentStages = AtomicInteger()
    val closedBeforeUse = AtomicInteger()
    val rejected = runCatching {
        completed.withResource({ closedBeforeUse.incrementAndGet() }) { subsequentStages.incrementAndGet() }
    }.exceptionOrNull()
    check(rejected is CancellationException && closedBeforeUse.get() == 1 && subsequentStages.get() == 0)

    val cancellation = RpcCancellation()
    val started = CountDownLatch(1)
    val stopped = CountDownLatch(1)
    val failure = AtomicReference<Throwable?>()
    val shutdowns = AtomicInteger()
    PipedInputStream().use { input ->
        PipedOutputStream(input).use { output ->
            val reader = Thread {
                try {
                    cancellation.withResource({ shutdowns.incrementAndGet(); output.close() }) {
                        started.countDown()
                        input.read()
                    }
                    subsequentStages.incrementAndGet()
                } catch (error: Throwable) {
                    failure.set(error)
                } finally {
                    stopped.countDown()
                }
            }.apply { isDaemon = true; start() }
            check(started.await(2, TimeUnit.SECONDS))
            cancellation.cancel()
            cancellation.cancel()
            check(stopped.await(2, TimeUnit.SECONDS)) { "Cancelled RPC read remained blocked" }
            reader.join(2000)
            check(!reader.isAlive && failure.get() is CancellationException)
            check(subsequentStages.get() == 0 && shutdowns.get() == 1)
        }
    }

    val hostSetsid = """
        setsid() {
            exec python3 -c 'import os, sys; os.setsid(); os.execvp(sys.argv[1], sys.argv[1:])' "${'$'}@"
        }
    """.trimIndent()
    fun startShell(command: String): Process = ProcessBuilder("/bin/sh", "-c", "$hostSetsid\n$command")
        .redirectErrorStream(true).start()
    // A su client may keep stdout open until its remote shell exits. The
    // completion marker must release the control pipe before waiting for EOF.
    fun startProxy(command: String): Process = startShell("""
        /bin/sh -c ${RootShell.quote("$hostSetsid\n$command")}
        dejavu_proxy_status=${'$'}?
        exit "${'$'}dejavu_proxy_status"
    """.trimIndent())
    for (starter in listOf(::startShell, ::startProxy)) {
        val result = RootShell.runCancellable("printf 'metadata-ready'; exit 7", RpcCancellation(), startProcess = starter)
        check(result.exitCode == 7 && result.output.trim() == "metadata-ready") {
            "Cancellable metadata collection changed output or exit status: $result"
        }
    }

    val began = System.nanoTime()
    repeat(8) {
        val metadataCancellation = RpcCancellation()
        val childStarted = CountDownLatch(1)
        val child = AtomicReference<Process?>()
        val metadataFailure = AtomicReference<Throwable?>()
        val reader = Thread {
            try {
                RootShell.runCancellable("exec sleep 30", metadataCancellation, startProcess = { command ->
                    (if (it % 2 == 0) startShell(command) else startProxy(command)).also { process ->
                        child.set(process)
                        childStarted.countDown()
                    }
                })
            } catch (error: Throwable) {
                metadataFailure.set(error)
            }
        }.apply { isDaemon = true; start() }
        try {
            check(childStarted.await(2, TimeUnit.SECONDS))
            metadataCancellation.cancel()
            reader.join(2000)
            check(!reader.isAlive && metadataFailure.get() is CancellationException) {
                "Superseded metadata request remained active: reader=${reader.isAlive}, " +
                    "shell=${child.get()?.isAlive}, failure=${metadataFailure.get()}"
            }
            check(child.get()!!.waitFor(2, TimeUnit.SECONDS)) { "Cancelled metadata shell survived" }
            var restarted = false
            check(runCatching {
                RootShell.runCancellable("printf stale", metadataCancellation, startProcess = {
                    restarted = true
                    startShell(it)
                })
            }.exceptionOrNull() is CancellationException)
            check(!restarted) { "Cancelled request started another metadata stage" }
        } finally {
            metadataCancellation.cancel()
            child.get()?.destroyForcibly()
        }
    }
    check(TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - began) < 5000) {
        "Rapidly cancelled metadata requests accumulated blocking work"
    }
    val groupFile = File.createTempFile("dejavu-metadata-group-", ".txt").also { it.delete() }
    val pipelineCancellation = RpcCancellation()
    val pipelineFailure = AtomicReference<Throwable?>()
    val pipelineProcess = AtomicReference<Process?>()
    val pipeline = Thread {
        try {
            RootShell.runCancellable("""
                printf '%s' "${'$'}${'$'}" > ${RootShell.quote(groupFile.path)}
                (sleep 30) | cat
            """.trimIndent(), pipelineCancellation, startProcess = { command ->
                startProxy(command).also(pipelineProcess::set)
            })
        } catch (error: Throwable) {
            pipelineFailure.set(error)
        }
    }.apply { isDaemon = true; start() }
    try {
        val readyDeadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2)
        while (groupFile.length() == 0L && System.nanoTime() < readyDeadline) Thread.sleep(10)
        val group = groupFile.readText().toLong()
        pipelineCancellation.cancel()
        pipeline.join(2000)
        check(!pipeline.isAlive && pipelineFailure.get() is CancellationException) {
            "Cancelled nested metadata pipeline remained blocked"
        }
        check(pipelineProcess.get()!!.waitFor(2, TimeUnit.SECONDS))
        // The pipeline's sleep and cat inherit this owned group. No member may
        // keep running or retain the output pipe after cancellation.
        val groupDeadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2)
        var alive: Boolean
        do {
            val probe = startShell("kill -0 -- -$group 2>/dev/null")
            alive = probe.waitFor(2, TimeUnit.SECONDS) && probe.exitValue() == 0
            if (alive) Thread.sleep(10)
        } while (alive && System.nanoTime() < groupDeadline)
        check(!alive) { "Cancelled metadata pipeline left its process group alive" }
    } finally {
        pipelineCancellation.cancel()
        pipelineProcess.get()?.destroyForcibly()
        groupFile.delete()
    }
    println("OK: RPC cancellation closes stalled reads, stops metadata jobs and rejects stale stages")
}
