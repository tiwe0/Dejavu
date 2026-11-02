package cafe.ivory.dejavu.data

import java.io.File

data class ShellResult(val exitCode: Int, val output: String)

/** Host-only bridge: every /data reference is mapped into one marked temp tree. */
object RootShell {
    private val sandbox = File(checkNotNull(System.getenv("DEJAVU_NATIVE_SANDBOX"))).canonicalFile.also {
        check(it.parentFile.canonicalFile == File(System.getProperty("java.io.tmpdir")).canonicalFile)
        check(it.name.startsWith("dejavu-native-regression."))
        check(File(it, ".host-only").readText() == "native-host-only\n")
    }
    val mapped = File(sandbox, "mapped-data")
    val properties = File(sandbox, "properties.json")
    fun quote(value: String): String = "'" + value.replace("'", "'\\''") + "'"
    fun start(command: String): Process = ProcessBuilder("/bin/sh", "-c",
        command.replace("/system/bin/flock", "flock").replace("/data", mapped.path))
        .redirectErrorStream(true).start()
    fun run(command: String): ShellResult = run(command, byteArrayOf())
    fun run(command: String, input: ByteArray): ShellResult {
        val process = start(command)
        process.outputStream.use { it.write(input) }
        val output = process.inputStream.bufferedReader().readText().replace(mapped.path, "/data")
        return ShellResult(process.waitFor(), output)
    }
}

const val DEFAULT_PLUGIN_DIRECTORY = "/data/adb/dejavu/plugins"
