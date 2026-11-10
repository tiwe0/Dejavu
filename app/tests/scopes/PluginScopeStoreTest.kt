package cafe.ivory.dejavu.data
import java.io.File
import java.util.concurrent.CountDownLatch
import java.util.concurrent.atomic.AtomicReference

fun main() {
    val directory = File(RootShell.mapped, "adb/modules/dejavu_zygisk/config").apply { mkdirs() }
    val file = File(directory, "targets.txt")
    val store = PluginScopeStore()
    fun fails(block: () -> Unit) { check(runCatching(block).isFailure) }
    file.writeText("# legacy\nmanual.app\n")
    check(store.read().scopes.isEmpty())
    store.setScope("first", listOf("shared.app", "first.app"))
    store.setScope("second", listOf("shared.app", "second.app"))
    check(store.read().targets == listOf("first.app", "manual.app", "second.app", "shared.app"))
    store.setScope("first", emptyList())
    check(store.read().targets == listOf("manual.app", "second.app", "shared.app"))
    store.removeManualTarget("shared.app")
    check("shared.app" in store.read().targets)
    store.removeManualTarget("manual.app")
    check("manual.app" !in store.read().targets)
    val count = RootShell.writes.get()
    RootShell.beforeCommit = { store.setScope("concurrent", listOf("concurrent.app")) }
    store.setScope("retry", listOf("retry.app"))
    check(RootShell.writes.get() >= count + 3)
    check(store.read().scopes.keys.containsAll(listOf("second", "concurrent", "retry")))
    val old = file.readBytes()
    File(RootShell.root, "fail-rename").writeText("")
    fails { store.setScope("failed", listOf("failed.app")) }
    check(old.contentEquals(file.readBytes()))
    check(directory.listFiles()!!.none { it.name.startsWith(".targets.") && it.name != ".targets.lock" })
    val gate = CountDownLatch(1)
    val failure = AtomicReference<Throwable?>()
    val threads = (1..2).map { number -> Thread {
        try { gate.await(); store.setScope("parallel$number", listOf("parallel$number.app")) }
        catch (error: Throwable) { failure.set(error) }
    }.apply { start() } }
    gate.countDown(); threads.forEach { it.join() }
    failure.get()?.let { throw it }
    check(store.read().scopes.keys.containsAll(listOf("parallel1", "parallel2")))
    RootShell.failRead = true
    fails { store.read() }; RootShell.failRead = false
    for (invalid in listOf("# dejavu-plugin-scopes-v2 {}\n", "# dejavu-plugin-scopes-v1 {}\n", "bad target\n", "#".repeat(16385))) {
        file.writeText(invalid); fails { store.read() }
    }
    file.writeBytes(old)
    fails { store.setScope("bad/id", emptyList()) }
    fails { store.setScope("large", (1..200).map { "a".repeat(110) + it }) }
    check(file.readBytes().contentEquals(old))
    val lock = File(directory, ".targets.lock")
    lock.delete()
    val victim = File(RootShell.root, "victim").apply { writeText("preserve") }
    java.nio.file.Files.createSymbolicLink(lock.toPath(), victim.toPath())
    fails { store.setScope("symlink", listOf("symlink.app")) }
    check(victim.readText() == "preserve" && file.readBytes().contentEquals(old))
    lock.delete()
    file.delete(); java.nio.file.Files.createSymbolicLink(file.toPath(), File(RootShell.root, "victim").toPath())
    fails { store.read() }
    println("PluginScopeStore: migration, independent/shared scopes, CAS retry, concurrency, atomic failure, validation, symlink and root errors passed")
}
