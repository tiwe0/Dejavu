package cafe.ivory.dejavu.data

import android.net.LocalSocket
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout

private const val RUNNING = "{\"installed\":true,\"running\":true,\"enabled\":true}"
private const val STOPPED = "{\"installed\":true,\"running\":false,\"enabled\":false}"
private fun endpoint() = """{"transport":"unix","namespace":"abstract","name":"dejavu.router.control","token":"${"a".repeat(64)}"}"""
private fun response(pid: Int) = """{"ok":true,"status":{"pid":$pid,"hub_connected":true,"plugin_count":1,"pending_jobs":0,"listen_port":39111,"hub_host":"localhost","hub_port":39010,"device_id":"test","token_configured":true,"routes":[]}}
"""
private fun CountDownLatch.awaitEvent(message: String) {
    check(await(3, TimeUnit.SECONDS)) { message }
}

/** Uses the production ViewModel and RouterClient with real coroutines and controlled socket/root IO. */
private class Fixture(
    shell: (String) -> ShellResult = { ShellResult(0, if (it.contains(" stop;")) STOPPED else RUNNING) },
    read: (String, RpcCancellation) -> ShellResult = { command, _ ->
        ShellResult(0, if (command.startsWith("cat ")) endpoint() else RUNNING)
    },
) : AutoCloseable {
    private val dispatcher = Executors.newSingleThreadExecutor().asCoroutineDispatcher()
    private val scope = CoroutineScope(SupervisorJob() + dispatcher)
    val model = RouterViewModel(RouterClient(shell, read), scope)
    init {
        LocalSocket.response = response(42)
        LocalSocket.uid = 0
        LocalSocket.onConnect = null
        LocalSocket.onRead = null
        LocalSocket.requests.clear()
    }
    fun main(block: RouterViewModel.() -> Unit) = runBlocking {
        withContext(dispatcher) { model.block() }
    }
    fun awaitState(message: String, predicate: (RouterUiState) -> Boolean): RouterUiState = runBlocking {
        try { withTimeout(3000) { model.state.first(predicate) } }
        catch (error: Exception) { throw AssertionError("$message; state=${model.state.value}", error) }
    }
    override fun close() {
        main { setVisible(false) }
        scope.cancel()
        dispatcher.close()
        LocalSocket.onConnect = null
        LocalSocket.onRead = null
    }
}

private fun rapidVisibilityCancelsRootDiscovery() {
    val entered = CountDownLatch(1)
    val cancelled = CountDownLatch(1)
    val reads = AtomicInteger()
    Fixture(read = { command, cancellation ->
        if (!command.startsWith("cat ") && reads.getAndIncrement() == 0) {
            cancellation.withResource({ cancelled.countDown() }) {
                entered.countDown()
                cancelled.awaitEvent("Hiding must cancel active root discovery")
                ShellResult(0, STOPPED)
            }
        } else ShellResult(0, if (command.startsWith("cat ")) endpoint() else RUNNING)
    }).use { fixture ->
        fixture.main { setVisible(true) }
        entered.awaitEvent("Discovery did not start")
        fixture.main { setVisible(false); setVisible(true) }
        cancelled.awaitEvent("Old discovery was not cancelled")
        fixture.awaitState("New visibility must recover without stale stopped state") {
            it.healthKnown && it.service?.running == true && it.snapshot?.pid == 42 && !it.loading
        }
        check(reads.get() == 2) { "One fresh lifecycle discovery per visibility generation" }
    }
}

private fun rapidVisibilityCancelsSocketRead() {
    val entered = CountDownLatch(1)
    val release = CountDownLatch(1)
    val held = AtomicReference<LocalSocket>()
    Fixture().use { fixture ->
        LocalSocket.onRead = { socket ->
            if (held.compareAndSet(null, socket)) {
                entered.countDown()
                release.awaitEvent("Release old socket read")
            }
        }
        try {
            fixture.main { setVisible(true) }
            entered.awaitEvent("Snapshot read did not start")
            fixture.main { setVisible(false); setVisible(true) }
            check(held.get().closed) { "Hiding must close an active polling socket" }
            LocalSocket.response = response(43)
        } finally { release.countDown() }
        fixture.awaitState("New visibility must publish the replacement snapshot") {
            it.snapshot?.pid == 43 && it.healthKnown && !it.loading
        }
    }
}

private fun hiddenRefreshIsCancelledBeforePublishing() {
    val entered = CountDownLatch(1)
    val release = CountDownLatch(1)
    val held = AtomicReference<LocalSocket>()
    Fixture().use { fixture ->
        fixture.main { setVisible(true) }
        fixture.awaitState("Initial snapshot") { it.snapshot?.pid == 42 && !it.loading }
        LocalSocket.onRead = { socket ->
            if (held.compareAndSet(null, socket)) {
                entered.countDown()
                release.awaitEvent("Release manual refresh")
            }
        }
        try {
            fixture.main { refresh() }
            entered.awaitEvent("Manual refresh did not start")
            fixture.main { setVisible(false) }
            check(held.get().closed) { "Hiding must also cancel a manual refresh read" }
        } finally { release.countDown() }
        fixture.awaitState("Cancelled refresh must not leave stale live data") {
            !it.loading && it.snapshot == null && it.failure == RouterFailure.Discovery
        }
    }
}

private fun queuedStopSurvivesPollingCancellation() {
    val entered = CountDownLatch(1)
    val release = CountDownLatch(1)
    val held = AtomicReference<LocalSocket>()
    val stops = AtomicInteger()
    Fixture(shell = { command ->
        check(command.contains(" stop;"))
        stops.incrementAndGet()
        ShellResult(0, STOPPED)
    }).use { fixture ->
        LocalSocket.onRead = { socket ->
            if (held.compareAndSet(null, socket)) {
                entered.countDown()
                release.awaitEvent("Release polling read before queued stop")
            }
        }
        try {
            fixture.main { setVisible(true) }
            entered.awaitEvent("Polling did not start")
            fixture.main { setRunning(false); setVisible(false) }
            check(held.get().closed)
        } finally { release.countDown() }
        fixture.awaitState("An authorized queued stop must finish while hidden") {
            it.service?.running == false && it.failure == null && !it.busy && !it.loading
        }
        check(stops.get() == 1)
    }
}

private fun lifecycleMutationSurvivesVisibilityChanges(running: Boolean) {
    val entered = CountDownLatch(1)
    val release = CountDownLatch(1)
    val calls = AtomicInteger()
    Fixture(shell = { command ->
        check(command.contains(if (running) " start;" else " stop;"))
        calls.incrementAndGet()
        entered.countDown()
        release.awaitEvent("Release authorized lifecycle action")
        ShellResult(0, if (running) RUNNING else STOPPED)
    }).use { fixture ->
        try {
            fixture.main { setRunning(running) }
            entered.awaitEvent("Lifecycle mutation did not start")
            fixture.main { setVisible(true); setVisible(false) }
            check(fixture.model.state.value.busy) { "Visibility must not finish a blocked lifecycle mutation" }
        } finally { release.countDown() }
        fixture.awaitState("Lifecycle action outcome must survive visibility changes") {
            it.service?.running == running && it.failure == null && !it.busy &&
                (!running || it.snapshot?.pid == 42)
        }
        check(calls.get() == 1)
    }
}

private fun offscreenRefreshReleasesQueuedStop() {
    val entered = CountDownLatch(1)
    val release = CountDownLatch(1)
    val stops = AtomicInteger()
    Fixture(
        shell = { command ->
            check(command.contains(" stop;"))
            stops.incrementAndGet()
            ShellResult(0, STOPPED)
        },
        read = { command, _ ->
            if (!command.startsWith("cat ")) {
                entered.countDown()
                release.awaitEvent("Release explicit offscreen refresh")
            }
            ShellResult(0, if (command.startsWith("cat ")) endpoint() else RUNNING)
        },
    ).use { fixture ->
        try {
            fixture.main { refresh() }
            entered.awaitEvent("Explicit offscreen refresh did not start")
            fixture.main { setRunning(false) }
            check(fixture.model.state.value.busy && stops.get() == 0) {
                "A queued lifecycle action must wait for the active refresh"
            }
        } finally { release.countDown() }
        fixture.awaitState("Offscreen refresh must release the queued stop") {
            it.service?.running == false && !it.loading && !it.busy && it.failure == null
        }
        check(stops.get() == 1)
    }
}

private fun visibilityDoesNotCancelAuthorizedControlMutation() {
    val entered = CountDownLatch(1)
    val release = CountDownLatch(1)
    val held = AtomicReference<LocalSocket>()
    Fixture().use { fixture ->
        fixture.main { setVisible(true) }
        fixture.awaitState("Initial snapshot") { it.snapshot?.pid == 42 && !it.loading }
        LocalSocket.onRead = { socket ->
            if (socket.outputStream.toString().contains("\"op\":\"configure\"") && held.compareAndSet(null, socket)) {
                entered.countDown()
                release.awaitEvent("Release authorized control mutation")
            }
        }
        try {
            fixture.main { configure("localhost", 39010, "changed", null) }
            entered.awaitEvent("Configure did not start")
            fixture.main { setVisible(false) }
            check(!held.get().closed) { "Hiding must not cancel an already authorized control mutation" }
        } finally { release.countDown() }
        fixture.awaitState("Control mutation must finish while hidden") {
            !it.busy && it.failure == null && it.snapshot?.pid == 42
        }
    }
}

fun main() {
    val tests = listOf(
        "rapid visibility cancels root discovery" to ::rapidVisibilityCancelsRootDiscovery,
        "rapid visibility cancels socket read" to ::rapidVisibilityCancelsSocketRead,
        "hidden refresh is cancelled before publishing" to ::hiddenRefreshIsCancelledBeforePublishing,
        "queued stop survives polling cancellation" to ::queuedStopSurvivesPollingCancellation,
        "start survives visibility changes" to { lifecycleMutationSurvivesVisibilityChanges(true) },
        "stop survives visibility changes" to { lifecycleMutationSurvivesVisibilityChanges(false) },
        "offscreen refresh releases queued stop" to ::offscreenRefreshReleasesQueuedStop,
        "visibility preserves authorized control mutation" to ::visibilityDoesNotCancelAuthorizedControlMutation,
    )
    for ((name, test) in tests) {
        test()
        println("PASS: $name")
    }
    println("Router ViewModel regression tests passed (${tests.size} tests)")
}
