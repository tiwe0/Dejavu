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
    fun select(process: String, names: Sequence<String>) = selectRpcEndpoint(process, names, setOf(123, 9))
    val endpoint = select(
        "com.example",
        sequenceOf("com.example.other.456.rpc", "com.example.123.rpc"),
    )
    check(endpoint == RpcEndpoint("com.example.123.rpc", 123))
    check(select("com.example", sequenceOf("comXexample.123.rpc")) == null)
    check(select("com.example", sequenceOf("com.example.0.rpc")) == null)
    check(select("com.example", sequenceOf("com.example.-1.rpc")) == null)
    check(select("com.example", sequenceOf("com.example.999999999999.rpc")) == null)
    check(select("com.example", sequenceOf("com.example.123.tmp.rpc")) == null)
    check(select("com.example:worker", sequenceOf("com.example:worker.9.rpc"))?.pid == 9)

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
    val scanning = initial.copy(loading = false, pluginDirectoryLoading = true, nativePluginActionPath = null)
        .withDirectoryPackages(listOf(imported))
    check(scanning.directoryPlugins.single().plugin == imported)
    check(scanning.directoryPlugins.single().runState == DirectoryPluginState.UNKNOWN)
    check(!scanning.pluginOperationInProgress) // background reads allow cards, tabs and back navigation
    check(!scanning.copy(loading = true).pluginOperationInProgress)
    check(scanning.copy(pluginImporting = true).pluginOperationInProgress)
    check(scanning.copy(pluginActionId = "example").pluginOperationInProgress)
    check(scanning.copy(nativePluginActionPath = "/plugin.so").pluginOperationInProgress)
    check(scanning.copy(scopeSavingId = "example").pluginOperationInProgress)
    val snapshot = mapOf("example" to PackageRuntimeSnapshot(DirectoryPluginState.RUNNING, "0123456789abcdef"))
    val runtimeFirst = initial.copy(directoryRuntimeSnapshot = snapshot)
        .withDirectoryPackages(listOf(imported))
    check(runtimeFirst.directoryPlugins.single().runState == DirectoryPluginState.RUNNING)
    check(runtimeFirst.directoryPlugins.single().session == "0123456789abcdef")
    val diskFirst = scanning.copy(directoryRuntimeSnapshot = snapshot)
        .withDirectoryPackages(listOf(imported))
    check(diskFirst.directoryPlugins == runtimeFirst.directoryPlugins)
    val emptyRuntime = scanning.copy(directoryRuntimeSnapshot = emptyMap())
        .withDirectoryPackages(listOf(imported))
    check(emptyRuntime.directoryPlugins.single().runState == DirectoryPluginState.STOPPED)
    val switchedDuringScan = diskFirst.withSelectedTarget("app.b")
    check(switchedDuringScan.pluginDirectoryLoading) // library scans are independent of target selection
    check(switchedDuringScan.directoryRuntimeSnapshot == null)
    check(switchedDuringScan.directoryPlugins.single().runState == DirectoryPluginState.UNKNOWN)
    check(switchedDuringScan.directoryPlugins.single().session == null)
    val viewer = PackageEventsUi(
        "example", "Example", "app.a", events = listOf(LivePluginEvent("test", "body", "now")),
        session = "0123456789abcdef",
    )
    check(viewer.matches("app.a", "example", "0123456789abcdef"))
    check(!viewer.matches("app.a", "example", "fedcba9876543210"))
    check(!viewer.matches("app.b", "example", "0123456789abcdef"))
    check(!viewer.matches("app.a", "example", null))
    val running = initial.copy(
        directoryPlugins = listOf(DirectoryPluginUi(imported, DirectoryPluginState.RUNNING, "0123456789abcdef")),
        packageEvents = viewer,
        pluginListenPorts = mapOf(("example" to "app.a") to 39001, ("example" to "app.b") to 39002),
        directoryTargetOffline = true,
        pluginActionId = "example", pluginDirectoryLoading = true,
    ).withSelectedTarget("app.b")
    check(running.directoryPlugins.single().runState == DirectoryPluginState.UNKNOWN)
    check(running.directoryPlugins.single().session == null && running.packageEvents == null)
    check(!running.directoryTargetOffline)
    check(running.pluginListenPorts["example" to "app.b"] == 39002)
    check(running.withSelectedTarget("app.a").pluginListenPorts["example" to "app.a"] == 39001)
    check(running.pluginActionId == null && running.pluginDirectoryLoading)
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
    val scoped = initial.copy(
        loading = false, activePluginId = "example",
        pluginScopes = mapOf("example" to listOf("app.a", "app.b"), "other" to listOf("app.b")),
    )
    check(scoped.canOperatePlugin("example", "app.a"))
    check(!scoped.canOperatePlugin("other", "app.a"))
    check(!scoped.canOperatePlugin("example", "app.c"))
    check(!scoped.canOperatePlugin("example", "app.b")) // selecting a scope is distinct from selecting the current RPC target
    check(scoped.withSelectedTarget("app.b").canOperatePlugin("example", "app.b"))
    check(!scoped.copy(scopeSavingId = "example").canOperatePlugin("example", "app.a"))
    check(scoped.copy(loading = true).canOperatePlugin("example", "app.a")) // read-only refresh must not suppress listener reconnection
    check(!scoped.copy(pluginScopes = emptyMap()).canOperatePlugin("example", "app.a"))
    check(scoped.withSelectedTarget(null).pluginScopes == scoped.pluginScopes)
    println("OK: exact RPC endpoint, selection reset and su resolution")
    runRpcRegressionTests()
    runPluginPackageRegressionTests()
    runPluginEventDecoderTests()
    runPluginEventProjectionTests()
    runPluginHookEventSocketTests()
}
