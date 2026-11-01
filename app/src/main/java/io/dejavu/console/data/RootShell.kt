package io.dejavu.console.data

import java.io.BufferedReader
import java.io.InputStreamReader
import java.util.concurrent.TimeUnit

data class ShellResult(val exitCode: Int, val output: String)

/** Tiny root-shell bridge. The app itself runs as an ordinary app UID. */
object RootShell {
    fun quote(value: String): String = "'" + value.replace("'", "'\\''") + "'"

    fun run(command: String, timeoutSeconds: Long = 20): ShellResult {
        val su = listOf(
            "/system/bin/su",
            "/debug_ramdisk/su",
            "/data/adb/ksu/bin/su",
            "/sbin/su",
        ).firstOrNull { java.io.File(it).canExecute() } ?: "su"
        val process = try {
            ProcessBuilder(su, "-c", command)
                .redirectErrorStream(true)
                .start()
        } catch (error: Exception) {
            return ShellResult(-1, "su is unavailable: ${error.message}")
        }
        val output = StringBuilder()
        val reader = Thread {
            try {
                process.inputStream.bufferedReader().use { stream: BufferedReader ->
                    stream.forEachLine { output.appendLine(it) }
                }
            } catch (_: Exception) {
            }
        }.apply { isDaemon = true; start() }
        val finished = process.waitFor(timeoutSeconds, TimeUnit.SECONDS)
        if (!finished) {
            process.destroy()
            process.waitFor(1, TimeUnit.SECONDS)
        }
        reader.join(500)
        return ShellResult(if (finished) process.exitValue() else 124, output.toString())
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
