package cafe.ivory.dejavu.data

fun main(args: Array<String>) {
    if (args.firstOrNull() == "--emit-packages") {
        require(args.size in 2..3) { "Usage: --emit-packages output-directory [session]" }
        emitPluginPackageScripts(
            java.io.File(System.getenv("DEJAVU_REPO_ROOT") ?: "."), java.io.File(args[1]),
            args.getOrElse(2) { "0123456789abcdef" },
        )
        return
    }
    val endpoint = selectRpcEndpoint(
        "com.example",
        sequenceOf("com.example.other.456.rpc", "com.example.123.rpc"),
    )
    check(endpoint == RpcEndpoint("com.example.123.rpc", 123))
    check(selectRpcEndpoint("com.example", sequenceOf("comXexample.123.rpc")) == null)
    check(selectRpcEndpoint("com.example", sequenceOf("com.example.0.rpc")) == null)
    check(selectRpcEndpoint("com.example", sequenceOf("com.example.-1.rpc")) == null)
    check(selectRpcEndpoint("com.example", sequenceOf("com.example.999999999999.rpc")) == null)
    check(selectRpcEndpoint("com.example", sequenceOf("com.example.123.tmp.rpc")) == null)
    check(selectRpcEndpoint("com.example:worker", sequenceOf("com.example:worker.9.rpc"))?.pid == 9)

    val plugin = PluginInfo("demo", "1", true, false, "0x0", false, "/demo.so")
    val initial = ConsoleUiState(
        selectedTarget = "app.a",
        pluginsLoading = true,
        pluginsRaw = "old plugin response",
        plugins = listOf(plugin),
        nativeLibrary = listOf(NativeLibraryPluginUi(
            NativePluginArtifact("/data/adb/dejavu/plugins/demo.so", "demo", false), plugin, true,
            error = "old target error",
        )),
        nativeRuntimeKnown = true,
        nativePluginActionPath = "/data/adb/dejavu/plugins/demo.so",
        targets = listOf("app.a", "app.b"),
    )
    val changed = initial.withSelectedTarget("app.b")
    check(changed.selectedTarget == "app.b")
    check(changed.plugins.isEmpty() && changed.pluginsRaw.isEmpty() && !changed.pluginsLoading)
    check(changed.nativeLibrary.single().runtime == null && !changed.nativeLibrary.single().runtimeKnown)
    check(changed.nativeLibrary.single().error == null)
    check(!changed.nativeRuntimeKnown && changed.nativePluginActionPath == null)
    check(changed.targets == initial.targets)
    check(initial.withSelectedTarget("app.a") == initial)
    check(initial.withSelectedTarget(null).plugins.isEmpty())
    val imported = ImportedPluginPackage(
        PluginPackageManifest("example", "Example", "1", entry = "entry.lua", files = listOf("entry.lua")),
        mapOf("entry.lua" to "return {}"),
    )
    val running = initial.copy(
        directoryPlugins = listOf(DirectoryPluginUi(imported, DirectoryPluginState.RUNNING, "0123456789abcdef")),
        packageEvents = PackageEventsUi("example", "Example", "app.a", events = listOf(LivePluginEvent("test", "body", "now"))),
        pluginActionId = "example", pluginDirectoryLoading = true,
    ).withSelectedTarget("app.b")
    check(running.directoryPlugins.single().runState == DirectoryPluginState.UNKNOWN)
    check(running.directoryPlugins.single().session == null && running.packageEvents == null)
    check(running.pluginActionId == null && !running.pluginDirectoryLoading)
    check(parsePackageRuntimeSnapshot("dejavu-packages-v1").isEmpty())
    check(parsePackageRuntimeSnapshot("dejavu-packages-v1\nexample\trunning\t0123456789abcdef")["example"]?.state == DirectoryPluginState.RUNNING)
    check(runCatching { parsePackageRuntimeSnapshot("invalid") }.isFailure)
    check(runCatching { parsePackageRuntimeSnapshot("dejavu-packages-v1\nexample\trunning\twrong") }.isFailure)

    val command = "logcat -v time -s DejavuPlugin:V"
    check(RootShell.commandArguments(command) { it == "/data/adb/ksu/bin/su" } ==
        listOf("/data/adb/ksu/bin/su", "-M", "-c", command))
    check(RootShell.commandArguments(command) { it == "/sbin/su" } ==
        listOf("/sbin/su", "-M", "-c", command))
    check(RootShell.commandArguments(command) { true }.first() == "/system/bin/su")
    check(RootShell.commandArguments(command) { false } == listOf("su", "-M", "-c", command))
    println("OK: exact RPC endpoint, selection reset and su resolution")
    runPluginPackageRegressionTests()
    runPluginEventDecoderTests()
    runPluginEventProjectionTests()
}
