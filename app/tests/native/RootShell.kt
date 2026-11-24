package cafe.ivory.dejavu.data

import java.io.File

/** Host-only bridge: every /data reference is mapped into one marked temp tree. */
object RootShell : ShellExecutor {
    var commandCount: Int = 0
        private set
    private val sandbox = File(checkNotNull(System.getenv("DEJAVU_NATIVE_SANDBOX"))).canonicalFile.also {
        check(it.parentFile.canonicalFile == File(System.getProperty("java.io.tmpdir")).canonicalFile)
        check(it.name.startsWith("dejavu-native-regression."))
        check(File(it, ".host-only").readText() == "native-host-only\n")
    }
    val mapped = File(sandbox, "mapped-data")
    val properties = File(sandbox, "properties.json")

    override fun start(command: String): Process {
        ++commandCount
        val paths = Regex("(?<![A-Za-z0-9_.-])/data(?=/|[\\s'\";]|$)")
            .replace(command.replace("/system/bin/flock", "flock")) { mapped.path }
        return ProcessBuilder("/bin/sh", "-c", paths)
            .redirectErrorStream(true).start()
    }

    override fun run(command: String, input: ByteArray, timeoutSeconds: Long): ShellResult {
        val process = start(command)
        process.outputStream.use { it.write(input) }
        val output = process.inputStream.bufferedReader().readText().replace(mapped.path, "/data")
        return ShellResult(process.waitFor(), output)
    }
}
