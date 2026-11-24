package cafe.ivory.dejavu.data

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import java.util.Base64
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

private class FakeEnvShell(initialTargets: String) : ShellExecutor {
    private val fileBytes = AtomicReference(initialTargets.toByteArray(Charsets.UTF_8))
    @Volatile var blockNextRead: CountDownLatch? = null
    @Volatile var readStarted: CountDownLatch? = null
    val helperOnline = AtomicBoolean(true)

    override fun start(command: String): Process =
        throw UnsupportedOperationException("start not used in EnvironmentViewModelTest")

    override fun run(command: String, input: ByteArray, timeoutSeconds: Long): ShellResult {
        return when {
            command == "id" -> ShellResult(0, "uid=0(root) gid=0(root)")
            command.startsWith("getprop ") && "debug.dejavu.root.port" in command ->
                ShellResult(0, if (helperOnline.get()) "18080\n" else "\n")
            command.startsWith("getprop ") && "debug.dejavu.root.token" in command ->
                ShellResult(0, if (helperOnline.get()) "${"a".repeat(64)}\n" else "\n")
            input.isNotEmpty() -> {
                fileBytes.set(input.copyOf())
                ShellResult(0, "")
            }
            "base64 <" in command -> {
                val snapshot = fileBytes.get()
                readStarted?.also {
                    readStarted = null
                    it.countDown()
                }
                blockNextRead?.also {
                    blockNextRead = null
                    check(it.await(5, TimeUnit.SECONDS)) { "Timed out waiting to release blocked read" }
                }
                ShellResult(0, Base64.getEncoder().encodeToString(snapshot))
            }
            else -> ShellResult(0, "")
        }
    }
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
    val initialTargets = "# dejavu-plugin-scopes-v1 {\"manual\":[\"manual.app\"],\"scopes\":{\"demo\":[\"old.app\"]}}\nmanual.app\nold.app\n"
    val shell = FakeEnvShell(initialTargets)
    val testScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    try {
        val vm = EnvironmentViewModel(
            shell = shell,
            scopeStore = PluginScopeStore(shell),
            loadAppsProvider = { listOf(InstalledApp("Demo", "new.app")) },
            strings = EnvironmentStrings(
                scopeSaved = { "scope-saved" },
                scopeWriteError = { "write-error:$it" },
                manualTargetRemoved = { "removed:$it" },
            ),
            coroutineScope = testScope,
        )

        // 1. Race guard: a slow refresh() reading stale scopes must NOT overwrite a newer savePluginScope().
        val readStarted = CountDownLatch(1)
        val releaseRead = CountDownLatch(1)
        shell.readStarted = readStarted
        shell.blockNextRead = releaseRead

        vm.refresh()
        check(readStarted.await(3, TimeUnit.SECONDS)) { "refresh did not reach scopeStore.read()" }

        val saveDone = CountDownLatch(1)
        vm.savePluginScope("demo", listOf("new.app")) { saveDone.countDown() }
        check(saveDone.await(3, TimeUnit.SECONDS)) { "savePluginScope did not finish" }
        check(vm.state.value.pluginScopes["demo"] == listOf("new.app"))

        // Release the in-flight refresh that already captured "old.app".
        releaseRead.countDown()
        waitUntil { !vm.state.value.loading && vm.state.value.rootGranted }
        check(vm.state.value.helperRunning && vm.state.value.helperPort == "18080")
        check(vm.state.value.pluginScopes["demo"] == listOf("new.app")) {
            "Stale refresh overwrote newer scope: ${vm.state.value.pluginScopes}"
        }
        check("new.app" in vm.state.value.targets && "old.app" !in vm.state.value.targets)

        // 2. removeManualTarget removes only the manual entry and posts confirmation message.
        vm.removeManualTarget("manual.app")
        waitUntil { "manual.app" !in vm.state.value.targets && vm.state.value.manualRemovingTarget == null }
        check(vm.state.value.message?.text == "removed:manual.app")
        check(vm.state.value.pluginScopes["demo"] == listOf("new.app"))

        // 3. Consecutive identical UiMessages get distinct monotonic IDs, and consuming an older ID preserves a newer one.
        vm.postMessage("same-text")
        val firstId = checkNotNull(vm.state.value.message?.id)
        vm.postMessage("same-text")
        val secondId = checkNotNull(vm.state.value.message?.id)
        check(secondId > firstId)
        vm.consumeMessage(firstId)
        check(vm.state.value.message?.id == secondId)
        vm.consumeMessage(secondId)
        check(vm.state.value.message == null)

        println("OK: EnvironmentViewModel scopeEpoch race guard, manual target removal, and monotonic UiMessage IDs")
    } finally {
        testScope.cancel()
    }
}
