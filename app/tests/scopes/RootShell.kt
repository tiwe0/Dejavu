package cafe.ivory.dejavu.data
import java.io.File
import java.util.concurrent.atomic.AtomicInteger

data class ShellResult(val exitCode: Int, val output: String)
object RootShell {
    val root = File(checkNotNull(System.getenv("DEJAVU_SCOPE_SANDBOX"))).canonicalFile.also {
        check(it.name.startsWith("dejavu-scopes."))
        check(File(it, ".host-only").readText() == "scopes-host-only\n")
    }
    val mapped = File(root, "data")
    var beforeCommit: (() -> Unit)? = null
    var failRead = false
    val writes = AtomicInteger()
    fun run(command: String): ShellResult = run(command, byteArrayOf())
    fun run(command: String, input: ByteArray): ShellResult {
        if (input.isEmpty() && failRead) return ShellResult(1, "root denied")
        if (input.isNotEmpty()) {
            writes.incrementAndGet()
            synchronized(this) { beforeCommit?.also { beforeCommit = null; it() } }
        }
        val process = ProcessBuilder("/bin/sh", "-c", command.replace("/system/bin/flock", "flock").replace("/data", mapped.path))
            .redirectErrorStream(true).start()
        process.outputStream.use { it.write(input) }
        val output = process.inputStream.bufferedReader().readText()
        return ShellResult(process.waitFor(), output)
    }
}
