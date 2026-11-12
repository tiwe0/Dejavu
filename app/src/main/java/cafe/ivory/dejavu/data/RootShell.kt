package cafe.ivory.dejavu.data

import java.io.BufferedReader
import java.util.UUID
import java.util.concurrent.CancellationException
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

data class ShellResult(val exitCode: Int, val output: String)

/** Cancels the current read-only RPC resource and rejects subsequent stages. */
internal class RpcCancellation {
    private val lock = Any()
    private var cancelled = false
    private var shutdown: (() -> Unit)? = null

    fun cancel() {
        val action = synchronized(lock) {
            if (cancelled) return
            cancelled = true
            shutdown.also { shutdown = null }
        }
        action?.let { runCatching(it) }
    }

    fun throwIfCancelled() {
        if (synchronized(lock) { cancelled }) throw CancellationException("RPC status request was cancelled")
    }

    fun <T> withResource(close: () -> Unit, action: () -> T): T {
        val alreadyCancelled = synchronized(lock) {
            if (cancelled) true else {
                check(shutdown == null) { "RPC resources must be used sequentially" }
                shutdown = close
                false
            }
        }
        if (alreadyCancelled) {
            runCatching(close)
            throwIfCancelled()
        }
        try {
            throwIfCancelled()
            val result = action()
            throwIfCancelled()
            return result
        } catch (error: Exception) {
            throwIfCancelled()
            throw error
        } finally {
            synchronized(lock) { if (shutdown === close) shutdown = null }
        }
    }
}

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

    internal fun runCancellable(
        command: String,
        cancellation: RpcCancellation,
        timeoutSeconds: Long = 20,
        startProcess: (String) -> Process = ::start,
    ): ShellResult {
        cancellation.throwIfCancelled()
        val completionMarker = "dejavu-metadata-complete-${UUID.randomUUID()}"
        // The root shell owns the command child. Closing our control pipe asks
        // that shell to stop its own job, even when the app cannot signal it.
        val process = try {
            val childCommand = """
                trap 'dejavu_status=$?; printf "\\n%s\\n" ${quote(completionMarker)}; exit "${'$'}dejavu_status"' EXIT
                $command
            """.trimIndent()
            startProcess("""
                setsid sh -c ${quote(childCommand)} &
                dejavu_child=${'$'}!
                exec 1>&- 2>&-
                if IFS= read -r dejavu_completion && [ "${'$'}dejavu_completion" = done ]; then
                    :
                else
                    dejavu_attempt=0
                    while [ "${'$'}dejavu_attempt" -lt 100 ]; do
                        kill -KILL -- "-${'$'}dejavu_child" 2>/dev/null && break
                        kill -0 "${'$'}dejavu_child" 2>/dev/null || break
                        dejavu_attempt=${'$'}((dejavu_attempt + 1))
                        sleep 0.01
                    done
                    kill -KILL %1 2>/dev/null
                fi
                wait "${'$'}dejavu_child"
            """.trimIndent())
        } catch (error: Exception) {
            cancellation.throwIfCancelled()
            return ShellResult(-1, "su is unavailable: ${error.message}")
        }
        return cancellation.withResource({ process.outputStream.close() }) {
            collectProcess(process, null, timeoutSeconds, completionMarker)
        }
    }

    private fun runWithInput(command: String, input: ByteArray?, timeoutSeconds: Long): ShellResult {
        val process = try {
            start(command)
        } catch (error: Exception) {
            return ShellResult(-1, "su is unavailable: ${error.message}")
        }
        return collectProcess(process, input, timeoutSeconds)
    }

    private fun collectProcess(
        process: Process,
        input: ByteArray?,
        timeoutSeconds: Long,
        completionMarker: String? = null,
    ): ShellResult {
        val output = StringBuilder()
        val inputFailure = AtomicReference<Exception?>()
        val reader = Thread {
            try {
                process.inputStream.bufferedReader().use { stream: BufferedReader ->
                    while (true) {
                        val line = stream.readLine() ?: break
                        if (completionMarker != null && line == completionMarker) break
                        synchronized(output) { output.appendLine(line) }
                    }
                }
            } catch (_: Exception) {
            } finally {
                if (completionMarker != null) runCatching {
                    process.outputStream.use { it.write("done\n".toByteArray()) }
                }
            }
        }.apply { isDaemon = true; start() }
        // Feed larger package resources over stdin instead of placing them in
        // su's argv. A separate writer keeps the process timeout effective.
        val writer = if (completionMarker != null) null else Thread {
            try {
                process.outputStream.use { stream -> if (input != null) stream.write(input) }
            } catch (error: Exception) {
                inputFailure.set(error)
            }
        }.apply { isDaemon = true; start() }
        val finished = process.waitFor(timeoutSeconds, TimeUnit.SECONDS)
        if (!finished) {
            if (completionMarker != null) runCatching { process.outputStream.close() }
            if (completionMarker == null || !process.waitFor(1, TimeUnit.SECONDS)) {
                process.destroy()
                if (!process.waitFor(1, TimeUnit.SECONDS)) process.destroyForcibly()
            }
        }
        reader.join(500)
        writer?.join(500)
        val text = synchronized(output) { output.toString() }
        if (finished && process.exitValue() == 0 && input != null &&
            (inputFailure.get() != null || writer?.isAlive == true)
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
