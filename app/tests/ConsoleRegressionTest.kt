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

    val plugin = PluginInfo("demo", "1", true, false, false, "/demo.so")
    val initial = PluginsUiState(
        selectedTarget = "app.a",
        pluginsLoading = true,
        plugins = listOf(plugin),
        nativeLibrary = listOf(NativeLibraryPluginUi(
            NativePluginArtifact("/data/adb/dejavu/plugins/demo.so", "demo", false), plugin, true,
            error = "old target error",
        )),
        nativeRuntimeKnown = true,
        nativePluginActionPath = "/data/adb/dejavu/plugins/demo.so",
    )
    val changed = initial.withSelectedTarget("app.b")
    check(changed.selectedTarget == "app.b")
    check(changed.plugins.isEmpty() && !changed.pluginsLoading)
    check(changed.nativeLibrary.single().runtime == null && !changed.nativeLibrary.single().runtimeKnown)
    check(changed.nativeLibrary.single().error == null)
    check(!changed.nativeRuntimeKnown && changed.nativePluginActionPath == null)
    check(initial.withSelectedTarget("app.a") == initial)
    check(initial.withSelectedTarget(null).plugins.isEmpty())
    val imported = ImportedPluginPackage(
        PluginPackageManifest("example", "Example", "1", entry = "entry.lua", files = listOf("entry.lua")),
        mapOf("entry.lua" to "return {}"),
    )
    val scanning = initial.copy(pluginDirectoryLoading = true, nativePluginActionPath = null)
        .withDirectoryPackages(listOf(imported))
    check(scanning.directoryPlugins.single().plugin == imported)
    check(scanning.directoryPlugins.single().runState == DirectoryPluginState.UNKNOWN)
    check(!scanning.operationInProgress(null)) // background reads allow cards, tabs and back navigation
    check(scanning.copy(pluginImporting = true).operationInProgress(null))
    check(scanning.copy(pluginActionId = "example").operationInProgress(null))
    check(scanning.copy(nativePluginActionPath = "/plugin.so").operationInProgress(null))
    check(scanning.operationInProgress("example"))
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
    val firstEvent = LivePluginEvent("test", "body", "now")
    val secondEvent = LivePluginEvent("test", "body", "now")
    check(secondEvent.id > firstEvent.id)
    val viewer = PackageEventsUi(
        "example", "Example", "app.a", events = listOf(firstEvent, secondEvent),
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
    val scopes = mapOf("example" to listOf("app.a", "app.b"), "other" to listOf("app.b"))
    val scoped = initial.copy(activePluginId = "example", nativePluginActionPath = null)
    check(scoped.canOperatePlugin("example", "app.a", scopes, null))
    check(!scoped.canOperatePlugin("other", "app.a", scopes, null))
    check(!scoped.canOperatePlugin("example", "app.c", scopes, null))
    check(!scoped.canOperatePlugin("example", "app.b", scopes, null)) // selecting a scope is distinct from selecting the current RPC target
    check(scoped.withSelectedTarget("app.b").canOperatePlugin("example", "app.b", scopes, null))
    check(!scoped.canOperatePlugin("example", "app.a", scopes, "example"))
    check(!scoped.canOperatePlugin("example", "app.a", emptyMap(), null))
    println("OK: exact RPC endpoint, selection reset and su resolution")
    runRpcRegressionTests()
    runPluginPackageRegressionTests()
    runPluginEventDecoderTests()
    runPluginEventProjectionTests()
    runPluginHookEventSocketTests()
}
