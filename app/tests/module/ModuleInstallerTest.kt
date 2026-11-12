package cafe.ivory.dejavu.data

import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.attribute.BasicFileAttributes
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import kotlin.concurrent.thread

private val properties = "id=dejavu_zygisk\nname=Dejavu Zygisk\nversion=v0.9.1\nversionCode=12\n"

private fun archive(prop: String = properties, extra: String = "lib/arm64-v8a/libdejavu_agent.so"): ByteArray {
    val output = ByteArrayOutputStream()
    ZipOutputStream(output).use { zip ->
        zip.putNextEntry(ZipEntry("module.prop")); zip.write(prop.toByteArray()); zip.closeEntry()
        zip.putNextEntry(ZipEntry(extra)); zip.write(byteArrayOf(0, 1, 2, -1, 13, 10)); zip.closeEntry()
    }
    return output.toByteArray()
}

private fun metadata(bytes: ByteArray): JSONObject = JSONObject().put("id", "dejavu_zygisk")
    .put("version", "v0.9.1").put("versionCode", 12).put("sha256", ModulePayload.checksum(bytes))
    .put("size", bytes.size)

private fun bundle(): ModuleBundle {
    val bytes = archive()
    return ModulePayload.read(bytes, metadata(bytes).toString())
}

private inline fun rejected(message: String, body: () -> Unit) {
    check(runCatching(body).isFailure) { message }
}

/** Executes the production shell text, redirecting only Android paths to an isolated fixture. */
private class TestRoot : AutoCloseable {
    val directory: Path = Files.createTempDirectory("dejavu-module-host-")
    private val bin = Files.createDirectory(directory.resolve("bin"))
    var identity = "1:APatch"
    var root = true
    var installerExit = 0
    var logLines = 0
    var logBytes = 0
    var blockInstaller = false
    var ignoreTerm = false
    var timeoutScale = "1"
    var transferCount = 0
    var shellCount = 0
    val received: Path = directory.resolve("received.zip")
    val invocation: Path = directory.resolve("invocation.txt")
    val client = ModuleInstaller(::shell, ::transfer, "su")

    init {
        executable(bin.resolve("id"), """
            [ "${'$'}1" = -u ] || exit 1
            printf '%s\n' "${'$'}DEJAVU_TEST_UID"
        """.trimIndent())
        executable(bin.resolve("su"), """
            [ "${'$'}1" = -v ] || exit 1
            printf '%s\n' "${'$'}DEJAVU_TEST_IDENTITY"
        """.trimIndent())
        executable(bin.resolve("timeout"), """
            import os, signal, subprocess, sys
            args = sys.argv[1:]
            assert args[0] == '-k'
            scale = float(os.environ.get('DEJAVU_TEST_TIMEOUT_SCALE', '1'))
            grace, duration = float(args[1]) * scale, float(args[2]) * scale
            process = subprocess.Popen(args[3:], start_new_session=True)
            try:
                status = process.wait(timeout=duration)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=grace)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                status = 124
            sys.exit(status if status >= 0 else 128 - status)
        """.trimIndent(), python = true)
        executable(bin.resolve("flock"), """
            import fcntl, sys
            operation = fcntl.LOCK_UN if '-u' in sys.argv else fcntl.LOCK_EX | fcntl.LOCK_NB
            try:
                fcntl.flock(int(sys.argv[-1]), operation)
            except BlockingIOError:
                sys.exit(1)
        """.trimIndent(), python = true)
        executable(bin.resolve("sha256sum"), "exec shasum -a 256 \"${'$'}@\"")
        val install = """
            case "${'$'}{0##*/}" in
                magisk)
                    if [ "${'$'}1" = --sqlite ]; then printf 'value=1\n'; exit 0; fi
                    [ "${'$'}1" = --install-module ] || exit 90
                    archive=${'$'}2 ;;
                *) [ "${'$'}1" = module ] && [ "${'$'}2" = install ] || exit 91; archive=${'$'}3 ;;
            esac
            printf '%s\n' "${'$'}{0##*/}" >> "${'$'}DEJAVU_TEST_DIR/invocation.txt"
            cp "${'$'}archive" "${'$'}DEJAVU_TEST_DIR/received.zip" || exit 92
            if [ "${'$'}DEJAVU_TEST_IGNORE_TERM" = 1 ]; then
                trap '' TERM
                printf '%s\n' "${'$'}${'$'}" > "${'$'}DEJAVU_TEST_DIR/installer.pid"
                while :; do sleep 0.02; done
            fi
            if [ "${'$'}DEJAVU_TEST_BLOCK" = 1 ]; then
                touch "${'$'}DEJAVU_TEST_DIR/installer-entered"
                while [ ! -f "${'$'}DEJAVU_TEST_DIR/release-installer" ]; do sleep 0.02; done
            fi
            awk -v count="${'$'}DEJAVU_TEST_LOG_LINES" 'BEGIN { for (i=0; i<count; i++) print "module installer log entry: " i }'
            if [ "${'$'}DEJAVU_TEST_LOG_BYTES" -gt 0 ]; then
                head -c "${'$'}DEJAVU_TEST_LOG_BYTES" /dev/zero | tr '\000' x
            fi
            if [ "${'$'}DEJAVU_TEST_INSTALL_EXIT" -ne 0 ]; then echo 'fixture installer failed'; exit "${'$'}DEJAVU_TEST_INSTALL_EXIT"; fi
            mkdir -p "${'$'}DEJAVU_TEST_DIR/modules_update/dejavu_zygisk" "${'$'}DEJAVU_TEST_DIR/modules/dejavu_zygisk"
            printf 'id=dejavu_zygisk\nversion=v0.9.1\nversionCode=12\n' > "${'$'}DEJAVU_TEST_DIR/modules_update/dejavu_zygisk/module.prop"
            touch "${'$'}DEJAVU_TEST_DIR/modules/dejavu_zygisk/update"
            echo 'fixture installer completed'
        """.trimIndent()
        // Deliberately retain all three installers to catch selection by file presence.
        executable(directory.resolve("apd"), install)
        executable(directory.resolve("ksud"), install)
        executable(bin.resolve("magisk"), install)
    }

    private fun executable(path: Path, script: String, python: Boolean = false) {
        Files.createDirectories(path.parent)
        Files.writeString(path, (if (python) "#!/usr/bin/env python3\n" else "#!/bin/sh\n") + script + "\n")
        check(path.toFile().setExecutable(true))
    }

    private fun run(script: String, input: ByteArray? = null, timeout: Long): ShellResult {
        val rewritten = script.replace("/data/adb", directory.toString())
        val process = ProcessBuilder("/bin/sh", "-c", rewritten).redirectErrorStream(true).apply {
            environment()["PATH"] = "$bin:${System.getenv("PATH")}"
            environment()["DEJAVU_TEST_IDENTITY"] = identity
            environment()["DEJAVU_TEST_UID"] = if (root) "0" else "20000"
            environment()["DEJAVU_TEST_DIR"] = directory.toString()
            environment()["DEJAVU_TEST_INSTALL_EXIT"] = installerExit.toString()
            environment()["DEJAVU_TEST_LOG_LINES"] = logLines.toString()
            environment()["DEJAVU_TEST_LOG_BYTES"] = logBytes.toString()
            environment()["DEJAVU_TEST_BLOCK"] = if (blockInstaller) "1" else "0"
            environment()["DEJAVU_TEST_IGNORE_TERM"] = if (ignoreTerm) "1" else "0"
            environment()["DEJAVU_TEST_TIMEOUT_SCALE"] = timeoutScale
        }.start()
        // A preflight refusal may close stdin before transfer; preserve its exit
        // status, matching the production RootShell rather than masking it as EPIPE.
        runCatching { process.outputStream.use { stream -> if (input != null) stream.write(input) } }
        val output = process.inputStream.bufferedReader().use { it.readText() }
        check(process.waitFor(timeout, TimeUnit.SECONDS)) { "Fixture shell timed out" }
        return ShellResult(process.exitValue(), output)
    }

    fun shell(script: String, timeout: Long): ShellResult {
        shellCount++
        return run(script, timeout = timeout)
    }

    fun transfer(script: String, input: ByteArray, timeout: Long): ShellResult {
        transferCount++
        check(timeout == ModuleInstaller.INSTALL_TIMEOUT_SECONDS)
        check("timeout -k 5 150" in script && "trap dejavu_cleanup EXIT" in script)
        check("sha256sum" in script && "mktemp -d /data/adb/.dejavu-install." in script)
        check("base64" !in script && !Regex("(?m)^\\s*(?:reboot|(?:\\S*ksud|\\S*apd)\\s+soft-reboot)(?:\\s|$)")
            .containsMatchIn(script)) { "No argv payload or automatic reboot" }
        return run(script, input, timeout)
    }

    fun noStagingRemains() {
        Files.list(directory).use { entries ->
            check(entries.noneMatch { it.fileName.toString().startsWith(".dejavu-install.") &&
                it.fileName.toString() != ".dejavu-install.lock" }) { "Private staging directory leaked" }
        }
    }

    override fun close() {
        directory.toFile().deleteRecursively()
    }
}

private fun payloadValidation() {
    val bytes = archive()
    val original = bytes.copyOf()
    val payload = ModulePayload.read(bytes, metadata(bytes).toString())
    bytes[0] = (bytes[0].toInt() xor 1).toByte()
    check(payload.archive.contentEquals(original)) { "Payload must own a verified archive snapshot" }
    val invalidMetadata = listOf(
        metadata(original).put("id", "another_module"),
        metadata(original).put("version", "v0.9.2"),
        metadata(original).put("versionCode", 13),
        metadata(original).put("size", original.size + 1),
        metadata(original).put("sha256", "f".repeat(64)),
        metadata(original).put("sha256", "NOT A CHECKSUM"),
    )
    invalidMetadata.forEach { value -> rejected("Invalid metadata accepted: $value") { ModulePayload.read(original, value.toString()) } }
    for (text in listOf(
        metadata(original).toString().replace("\"versionCode\":12", "\"versionCode\":12.0"),
        metadata(original).toString().replace("\"size\":${original.size}", "\"size\":${original.size}.0"),
    )) rejected("Non-integral JSON type accepted") { ModulePayload.read(original, text) }
    for (path in listOf("../outside", "/absolute", "dir/../escape", "dir\\escape", "C:escape", "dir//empty", "dir/./entry")) {
        val unsafe = archive(extra = path)
        rejected("Unsafe path accepted: $path") { ModulePayload.read(unsafe, metadata(unsafe).toString()) }
    }
    for (prop in listOf(properties.replace("dejavu_zygisk", "other"), properties + "version=v0.9.1\n", "id=dejavu_zygisk\n")) {
        val invalid = archive(prop)
        rejected("Wrong module properties accepted") { ModulePayload.read(invalid, metadata(invalid).toString()) }
    }
    val hugeProp = archive(properties + "description=" + "x".repeat(65536))
    rejected("Oversized properties accepted") { ModulePayload.read(hugeProp, metadata(hugeProp).toString()) }
    val collisionOutput = ByteArrayOutputStream()
    ZipOutputStream(collisionOutput).use { zip ->
        for ((name, content) in listOf("module.prop" to properties, "same/" to "", "same" to "file")) {
            zip.putNextEntry(ZipEntry(name)); zip.write(content.toByteArray()); zip.closeEntry()
        }
    }
    val collision = collisionOutput.toByteArray()
    rejected("Archive directory/file collision accepted") { ModulePayload.read(collision, metadata(collision).toString()) }
    rejected("Empty archive accepted") { ModulePayload.read(byteArrayOf(), "{}") }
    println("PASS: bundled ZIP identity/version, checksum, strict metadata and safe path/size validation")
}

private fun providerInstallations() {
    for ((identity, provider, installer) in listOf(
        Triple("11224:APatch", RootProvider.APATCH, "apd"),
        Triple("115032-Matsuzaka-yuki:APatch(FolkPatch)", RootProvider.APATCH, "apd"),
        Triple("123:KernelSU", RootProvider.KERNELSU, "ksud"),
        Triple("27000:MAGISKSU", RootProvider.MAGISK, "magisk"),
    )) TestRoot().use { root ->
        root.identity = identity
        val state = root.client.inspect()
        check(state.rootGranted && state.provider == provider)
        val payload = bundle()
        val result = root.client.install(payload, 35, listOf("arm64-v8a", "armeabi-v7a"))
        check(result.exitCode == 0 && "reboot is required" in result.output) { result.output }
        check(Files.readString(root.invocation).trim() == installer) { "Selected a residual installer instead of the authorized root provider" }
        check(Files.readAllBytes(root.received).contentEquals(payload.archive)) { "Binary stdin transfer changed archive bytes" }
        val after = root.client.inspect()
        check(after.rebootPending && after.stagedVersion == "v0.9.1")
        root.noStagingRemains()
        println("PASS: $provider identity selects its own installer, binary stdin and pending-reboot status")
    }
}

private fun inspectionStates() = TestRoot().use { root ->
    val module = Files.createDirectories(root.directory.resolve("modules/dejavu_zygisk"))
    Files.writeString(module.resolve("module.prop"), "version=v0.8.2\n")
    Files.createFile(module.resolve("disable")); Files.createFile(module.resolve("remove"))
    val provider = Files.createDirectories(root.directory.resolve("modules/zygisksu"))
    val state = root.client.inspect()
    check(state.installedVersion == "v0.8.2" && state.disabled && state.removalPending)
    check(state.rebootPending && state.zygiskPresent)
    Files.createFile(provider.resolve("disable"))
    check(!root.client.inspect().zygiskPresent)
    root.root = false
    check(!root.client.inspect().rootGranted)
    println("PASS: installed/staged, disable/remove/update and Zygisk provider states stay distinct")
}

private fun preflightAndTampering() {
    TestRoot().use { root ->
        val payload = bundle()
        for ((sdk, abis) in listOf(25 to listOf("arm64-v8a"), 36 to listOf("arm64-v8a"), 35 to listOf("x86_64"))) {
            check(root.client.install(payload, sdk, abis).exitCode != 0)
        }
        check(root.shellCount == 0 && root.transferCount == 0) { "Unsupported devices must fail before root requests" }
        payload.archive[0] = 0
        check(root.client.install(payload, 35, listOf("arm64-v8a")).exitCode != 0)
        check(root.shellCount == 0 && root.transferCount == 0) { "Mutable payload tampering must fail before root" }
    }
    TestRoot().use { root ->
        root.root = false
        check(root.client.install(bundle(), 35, listOf("arm64-v8a")).exitCode != 0)
        check(root.transferCount == 0)
    }
    TestRoot().use { root ->
        root.identity = "unknown-su"
        check(root.client.inspect().provider == null)
        check(root.client.install(bundle(), 35, listOf("arm64-v8a")).exitCode != 0)
        check(root.transferCount == 0) { "Existing installer binaries cannot authorize an unknown su provider" }
    }
    TestRoot().use { root ->
        Files.delete(root.directory.resolve("apd"))
        val result = root.client.install(bundle(), 35, listOf("arm64-v8a"))
        check(result.exitCode != 0 && "installer is unavailable" in result.output)
        check(!Files.exists(root.received)) { "Missing provider installer must not fall back to another root manager" }
        root.noStagingRemains()
    }
    TestRoot().use { root ->
        val client = ModuleInstaller(root::shell, { script, input, timeout ->
            root.identity = "1:KernelSU"
            root.transfer(script, input, timeout)
        }, "su")
        check(client.install(bundle(), 35, listOf("arm64-v8a")).exitCode != 0)
        check(!Files.exists(root.received)) { "Provider identity changed after discovery" }
        root.noStagingRemains()
    }
    TestRoot().use { root ->
        val client = ModuleInstaller(root::shell, { script, input, timeout ->
            val corrupted = input.copyOf().apply { this[0] = 0 }
            root.transfer(script, corrupted, timeout)
        }, "su")
        val result = client.install(bundle(), 35, listOf("arm64-v8a"))
        check(result.exitCode != 0 && "checksum mismatch" in result.output)
        check(!Files.exists(root.received)) { "Installer ran on corrupted root payload" }
        root.noStagingRemains()
    }
    println("PASS: SDK/ABI, mutated payload, missing root, unknown/changed provider and root-side checksum guards")
}

private fun failedAndBoundedInstallation() {
    TestRoot().use { root ->
        root.installerExit = 7
        val result = root.client.install(bundle(), 35, listOf("arm64-v8a"))
        check(result.exitCode == 7 && "fixture installer failed" in result.output) { result.toString() }
        check(!root.client.inspect().rebootPending)
        root.noStagingRemains()
    }
    TestRoot().use { root ->
        root.installerExit = 124
        val result = root.client.install(bundle(), 35, listOf("arm64-v8a"))
        check(result.exitCode == 124) { "Installer timeout became success" }
        root.noStagingRemains()
    }
    TestRoot().use { root ->
        root.logLines = 16000
        val result = root.client.install(bundle(), 35, listOf("arm64-v8a"))
        check(result.exitCode == 0 && result.output.length <= ModuleInstaller.MAX_LOG_CHARS) { "Installer output was unbounded or lost its exit code" }
        check(Files.exists(root.received))
        root.noStagingRemains()
    }
    for (exit in listOf(0, 7)) TestRoot().use { root ->
        root.logBytes = 32 * 1024 * 1024
        root.installerExit = exit
        val result = root.client.install(bundle(), 35, listOf("arm64-v8a"))
        check(result.exitCode == exit && result.output.length <= ModuleInstaller.MAX_LOG_CHARS) {
            "Newline-free log flood lost completion status or exceeded the output bound"
        }
        check(root.client.inspect().rebootPending == (exit == 0)) {
            "Capping output interrupted the real installer before it completed"
        }
        root.noStagingRemains()
    }
    println("PASS: failures/timeouts and 32 MiB newline-free logs retain exit status, drain fully and clean staging")
}

private fun concurrentInstallAndProtocol() {
    val entered = CountDownLatch(1); val released = CountDownLatch(1)
    val reads = AtomicInteger()
    val client = ModuleInstaller({ _, _ ->
        reads.incrementAndGet(); ShellResult(0, "root=1\nprovider=APATCH\n")
    }, { script, _, _ ->
        entered.countDown()
        check(released.await(3, TimeUnit.SECONDS))
        val marker = Regex("DEJAVU_INSTALL_EXIT_[0-9a-f]+").find(script)!!.value
        ShellResult(0, "installed\n$marker=0\n")
    }, "su")
    val payload = bundle()
    var first: ShellResult? = null
    val job = thread { first = client.install(payload, 35, listOf("arm64-v8a")) }
    check(entered.await(2, TimeUnit.SECONDS))
    check(client.install(payload, 35, listOf("arm64-v8a")).exitCode != 0)
    check(reads.get() == 1) { "Concurrent install must not request root or start a second transfer" }
    released.countDown(); job.join(3000)
    check(!job.isAlive && first?.exitCode == 0)
    val missingCompletion = ModuleInstaller({ _, _ -> ShellResult(0, "root=1\nprovider=APATCH\n") },
        { _, _, _ -> ShellResult(0, "installer output without terminal status") }, "su")
    check(missingCompletion.install(payload, 35, listOf("arm64-v8a")).exitCode != 0)
    println("PASS: duplicate installs are serialized and missing terminal status cannot report success")
}

private fun inspectionDiagnostics() {
    for (exit in listOf(-1, 1, 124)) {
        val client = ModuleInstaller({ _, _ -> ShellResult(exit, "root transport diagnostic " + "x".repeat(4000)) },
            { _, _, _ -> error("Failed discovery cannot transfer a module") }, "su")
        val failure = runCatching { client.inspect() }.exceptionOrNull()
        check(failure is IOException && "exit $exit" in failure.message.orEmpty())
        check(failure.message.orEmpty().length < 1200) { "Inspection diagnostics must remain bounded" }
        val result = client.install(bundle(), 35, listOf("arm64-v8a"))
        check(result.exitCode != 0 && "exit $exit" in result.output && "authorization is required" !in result.output)
    }
    for (output in listOf("", "provider=APATCH\n", "root=2\nprovider=APATCH\n",
        "root=1\nroot=0\nprovider=APATCH\n", "root=1\n", "root=1\nprovider=BAD\n",
        "root=1\nprovider=APATCH\ninstalling=maybe\n")) {
        val client = ModuleInstaller({ _, _ -> ShellResult(0, output) }, { _, _, _ -> error("No transfer") }, "su")
        check(runCatching { client.inspect() }.exceptionOrNull() is IOException) { "Malformed snapshot accepted: $output" }
    }
    val denied = ModuleInstaller({ _, _ -> ShellResult(0, "root=0\n") }, { _, _, _ -> error("No transfer") }, "su")
    check(!denied.inspect().rootGranted)
    println("PASS: failed/timed-out inspection retains bounded diagnostics; only explicit root=0 means missing authorization")
}

private fun pendingTransactionGuards() {
    for (pending in listOf("update", "remove", "staged")) TestRoot().use { root ->
        if (pending == "staged") {
            Files.createDirectories(root.directory.resolve("modules_update/dejavu_zygisk"))
        } else {
            val module = Files.createDirectories(root.directory.resolve("modules/dejavu_zygisk"))
            Files.createFile(module.resolve(pending))
        }
        check(root.client.inspect().rebootPending)
        check(root.client.install(bundle(), 35, listOf("arm64-v8a")).exitCode == 75)
        check(root.transferCount == 0) { "Pending change should refuse before transfer" }
        // Simulate a stale UI/discovery snapshot; the root lock's fresh preflight
        // must still preserve the real staged/update/remove transaction.
        val stale = ModuleInstaller({ script, timeout ->
            val result = root.shell(script, timeout)
            result.copy(output = result.output.replace("pending=1", "pending=0")
                .replace("update=1", "update=0").replace("remove=1", "remove=0"))
        }, root::transfer, "su")
        check(stale.install(bundle(), 35, listOf("arm64-v8a")).exitCode == 75)
        check(!Files.exists(root.received)) { "Stale inspection overwrote a pending module change" }
        root.noStagingRemains()
    }
    println("PASS: active remove/update and incomplete staged transactions are rechecked under the root lock")
}

private fun processWideInstallLock() = TestRoot().use { root ->
    root.blockInstaller = true
    check(!root.client.inspect().installationRunning)
    val lock = root.directory.resolve(".dejavu-install.lock")
    val identity = Files.readAttributes(lock, BasicFileAttributes::class.java).fileKey()
    val payload = bundle()
    var first: ShellResult? = null
    val job = thread { first = root.client.install(payload, 35, listOf("arm64-v8a")) }
    val entered = root.directory.resolve("installer-entered")
    val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(4)
    while (!Files.exists(entered) && System.nanoTime() < deadline) Thread.sleep(10)
    check(Files.exists(entered)) { "First real installer process did not enter" }
    try {
        check(root.client.inspect().installationRunning) { "A lock file alone is not the running-state evidence" }
        val reopened = ModuleInstaller(root::shell, root::transfer, "su")
        check(reopened.install(payload, 35, listOf("arm64-v8a")).exitCode == 75)
        val stale = ModuleInstaller({ script, timeout ->
            root.shell(script, timeout).let { it.copy(output = it.output.replace("installing=1", "installing=0")) }
        }, root::transfer, "su")
        check(stale.install(payload, 35, listOf("arm64-v8a")).exitCode == 75) { "Root script must serialize even a stale clear snapshot" }
        check(Files.readAllLines(root.invocation).size == 1) { "Multiple installer processes ran concurrently" }
    } finally {
        Files.createFile(root.directory.resolve("release-installer"))
        job.join(5000)
    }
    check(!job.isAlive && first?.exitCode == 0)
    check(!root.client.inspect().installationRunning)
    check(Files.readAttributes(lock, BasicFileAttributes::class.java).fileKey() == identity) { "Lock inode was replaced/unlinked" }
    root.noStagingRemains()
    println("PASS: independent clients and root shell processes share a stable flock inode; only one installer executes")
}

private fun realTermIgnoringTimeout() = TestRoot().use { root ->
    root.ignoreTerm = true
    root.timeoutScale = "0.01" // Production 150s/5s becomes 1.5s/0.05s in the portable timeout fixture.
    val started = System.nanoTime()
    val result = root.client.install(bundle(), 35, listOf("arm64-v8a"))
    val elapsed = TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - started)
    check(result.exitCode == 124 && elapsed in 1000..7000) { "TERM-ignore installer was not killed within the grace deadline: $result, ${elapsed}ms" }
    val pid = Files.readString(root.directory.resolve("installer.pid")).trim().toLong()
    check(ProcessHandle.of(pid).map { !it.isAlive }.orElse(true)) { "Timed-out root installer still runs" }
    check(!root.client.inspect().installationRunning) { "Timeout did not release the process-wide lock" }
    root.noStagingRemains()
    println("PASS: real TERM-ignoring installer is KILLed after grace; private stage is removed and root lock releases")
}

fun main() {
    payloadValidation()
    providerInstallations()
    inspectionStates()
    preflightAndTampering()
    failedAndBoundedInstallation()
    concurrentInstallAndProtocol()
    inspectionDiagnostics()
    pendingTransactionGuards()
    processWideInstallLock()
    realTermIgnoringTimeout()
    println("Module installer host regression passed")
}
