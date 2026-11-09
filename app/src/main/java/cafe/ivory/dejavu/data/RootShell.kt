package cafe.ivory.dejavu.data

import java.io.BufferedReader
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

data class ShellResult(val exitCode: Int, val output: String)

/** Tiny root-shell bridge. The app itself runs as an ordinary app UID. */
object RootShell {
    fun quote(value: String): String = "'" + value.replace("'", "'\\''") + "'"

    internal fun commandArguments(
        command: String,
        canExecute: (String) -> Boolean = { java.io.File(it).canExecute() },
    ): List<String> {
        val su = listOf(
            "/system/bin/su",
            "/debug_ramdisk/su",
            "/data/adb/ksu/bin/su",
            "/sbin/su",
        ).firstOrNull(canExecute) ?: "su"
        // App mount namespaces can hide other targets' private directories.
        return listOf(su, "-M", "-c", command)
    }

    fun start(command: String): Process = ProcessBuilder(commandArguments(command))
        .redirectErrorStream(true)
        .start()

    // The app cannot signal a root child. The shell reaps its own logcat
    // when the app closes stdin, including when the app process exits.
    internal fun startLogcat(arguments: String): Process = start(
        """
        logcat $arguments &
        exec 1>&- 2>&-
        IFS= read -r stop
        kill %1
        wait
        """.trimIndent(),
    )

    internal fun stopLogcat(process: Process?) {
        runCatching { process?.outputStream?.close() }
    }

    fun run(command: String, timeoutSeconds: Long = 20): ShellResult =
        runWithInput(command, null, timeoutSeconds)

    fun run(command: String, input: ByteArray, timeoutSeconds: Long = 20): ShellResult =
        runWithInput(command, input, timeoutSeconds)

    private fun runWithInput(command: String, input: ByteArray?, timeoutSeconds: Long): ShellResult {
        val process = try {
            start(command)
        } catch (error: Exception) {
            return ShellResult(-1, "su is unavailable: ${error.message}")
        }
        val output = StringBuilder()
        val inputFailure = AtomicReference<Exception?>()
        val reader = Thread {
            try {
                process.inputStream.bufferedReader().use { stream: BufferedReader ->
                    stream.forEachLine { line -> synchronized(output) { output.appendLine(line) } }
                }
            } catch (_: Exception) {
            }
        }.apply { isDaemon = true; start() }
        // Feed larger package resources over stdin instead of placing them in
        // su's argv. A separate writer keeps the process timeout effective.
        val writer = Thread {
            try {
                process.outputStream.use { stream -> if (input != null) stream.write(input) }
            } catch (error: Exception) {
                inputFailure.set(error)
            }
        }.apply { isDaemon = true; start() }
        val finished = process.waitFor(timeoutSeconds, TimeUnit.SECONDS)
        if (!finished) {
            process.destroy()
            if (!process.waitFor(1, TimeUnit.SECONDS)) process.destroyForcibly()
        }
        reader.join(500)
        writer.join(500)
        val text = synchronized(output) { output.toString() }
        if (finished && process.exitValue() == 0 && input != null &&
            (inputFailure.get() != null || writer.isAlive)
        ) {
            return ShellResult(-1, "Unable to send root command input\n$text")
        }
        return ShellResult(if (finished) process.exitValue() else 124, text)
    }

    fun getProp(name: String): String {
        val result = run("getprop ${quote(name)}")
        return if (result.exitCode == 0) result.output.trim().lineSequence().firstOrNull().orEmpty()
        else ""
    }

    fun readFile(path: String): String? {
        val result = run("cat ${quote(path)}")
        return if (result.exitCode == 0) result.output else null
    }

    fun writeFile(path: String, contents: String): Boolean {
        val escaped = contents
        val command = "printf %s ${quote(escaped)} > ${quote(path)}"
        return run(command).exitCode == 0
    }

    fun isRoot(): Boolean = run("id").output.contains("uid=0")
}
