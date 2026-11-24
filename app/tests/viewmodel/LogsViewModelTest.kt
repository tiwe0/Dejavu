package cafe.ivory.dejavu.data

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import java.io.ByteArrayInputStream
import java.io.InputStream
import java.io.OutputStream
import java.io.PipedInputStream
import java.io.PipedOutputStream
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

private class FakeLogcatProcess : Process() {
    val pipeOut = PipedOutputStream()
    private val pipeIn = PipedInputStream(pipeOut, 256 * 1024)
    private val alive = AtomicBoolean(true)

    fun emitLine(line: String) {
        pipeOut.write((line + "\n").toByteArray(Charsets.UTF_8))
        pipeOut.flush()
    }

    private val stdinControl = object : OutputStream() {
        override fun write(b: Int) = Unit
        override fun close() {
            destroy()
        }
    }

    override fun getOutputStream(): OutputStream = stdinControl
    override fun getInputStream(): InputStream = pipeIn
    override fun getErrorStream(): InputStream = ByteArrayInputStream(ByteArray(0))
    override fun waitFor(): Int = 0
    override fun exitValue(): Int = 0
    override fun isAlive(): Boolean = alive.get()

    override fun destroy() {
        if (alive.compareAndSet(true, false)) {
            runCatching { pipeOut.close() }
            runCatching { pipeIn.close() }
        }
    }

    override fun destroyForcibly(): Process {
        destroy()
        return this
    }
}

private class FakeLogShell : ShellExecutor {
    val startedCount = AtomicInteger(0)
    val processes = CopyOnWriteArrayList<FakeLogcatProcess>()

    override fun start(command: String): Process {
        startedCount.incrementAndGet()
        return FakeLogcatProcess().also { processes += it }
    }

    override fun run(command: String, input: ByteArray, timeoutSeconds: Long): ShellResult = ShellResult(0, "")
}

private fun waitUntil(timeoutMs: Long = 3_000, predicate: () -> Boolean) {
    val deadline = System.currentTimeMillis() + timeoutMs
    while (System.currentTimeMillis() < deadline) {
        if (predicate()) return
        Thread.sleep(10)
    }
    error("Condition not satisfied within ${timeoutMs}ms")
}

fun main() {
    val shell = FakeLogShell()
    val testScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    try {
        val vm = LogsViewModel(
            shell = shell,
            excludedMarkersProvider = { setOf("secret_marker") },
            coroutineScope = testScope,
            flushIntervalMs = 40L,
        )

        // 1. Visibility starts logcat; short bursts flush via the trailing timer even without follow-up lines.
        vm.setVisible(true)
        waitUntil { shell.processes.size == 1 && vm.state.value.running }
        val firstProcess = shell.processes[0]
        firstProcess.emitLine("01-01 00:00:01.000 I/DejavuAgent: burst line 1")
        firstProcess.emitLine("01-01 00:00:01.001 I/DejavuAgent: skip secret_marker line")
        firstProcess.emitLine("01-01 00:00:01.002 I/DejavuAgent: burst line 2")
        waitUntil { "burst line 2" in vm.state.value.logText }
        check("burst line 1" in vm.state.value.logText)
        check("secret_marker" !in vm.state.value.logText)
        val versionAfterBurst = vm.state.value.logVersion
        check(versionAfterBurst > 0L)

        // 2. Exceeding the 80 KB cap keeps logText bounded while still bumping logVersion for auto-scroll.
        val chunk = "X".repeat(2_000)
        repeat(45) { idx ->
            firstProcess.emitLine("line-$idx-$chunk")
        }
        waitUntil { "line-44-" in vm.state.value.logText }
        check(vm.state.value.logText.length <= 80_000)
        val versionAtCap = vm.state.value.logVersion
        firstProcess.emitLine("line-after-cap-$chunk")
        waitUntil { "line-after-cap-" in vm.state.value.logText }
        check(vm.state.value.logText.length <= 80_000)
        check(vm.state.value.logVersion > versionAtCap)

        // 3. Hiding stops logcat; returning resumes automatically because user did not explicitly stop.
        vm.setVisible(false)
        waitUntil { !vm.state.value.running && !firstProcess.isAlive }
        vm.setVisible(true)
        waitUntil { shell.startedCount.get() == 2 && vm.state.value.running }

        // 4. Explicit stop() prevents auto-restart on hide -> show until start() is invoked.
        vm.stop()
        waitUntil { !vm.state.value.running }
        vm.setVisible(false)
        vm.setVisible(true)
        Thread.sleep(80)
        check(shell.startedCount.get() == 2)
        check(!vm.state.value.running)

        vm.start()
        waitUntil { shell.startedCount.get() == 3 && vm.state.value.running }
        check(vm.state.value.logText.isEmpty())

        vm.stop()
        waitUntil { !vm.state.value.running }
        println("OK: LogsViewModel trailing flush, 80KB cap logVersion bump, and visibility/userStopped lifecycle")
    } finally {
        testScope.cancel()
    }
}
