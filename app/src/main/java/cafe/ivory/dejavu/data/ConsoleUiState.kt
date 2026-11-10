package cafe.ivory.dejavu.data

import android.graphics.Bitmap

data class InstalledApp(
    val label: String,
    val packageName: String,
    val icon: Bitmap? = null,
)

data class PluginInfo(
    val name: String,
    val version: String,
    val loaded: Boolean,
    val jit: Boolean,
    val capabilities: String,
    val skill: Boolean,
    val path: String,
)

data class NativeLibraryPluginUi(
    val artifact: NativePluginArtifact,
    val runtime: PluginInfo? = null,
    val runtimeKnown: Boolean = false,
    val error: String? = null,
)

data class ConsoleUiState(
    val loading: Boolean = true,
    val pluginsLoading: Boolean = false,
    val rootGranted: Boolean = false,
    val moduleInstalled: Boolean = false,
    val moduleVersion: String = "",
    val helperRunning: Boolean = false,
    val helperPort: String = "",
    val helperPid: String = "",
    val targets: List<String> = emptyList(),
    val selectedTarget: String? = null,
    val activePluginId: String? = null,
    val pluginScopes: Map<String, List<String>> = emptyMap(),
    val scopeSavingId: String? = null,
    val pluginsRaw: String = "",
    val plugins: List<PluginInfo> = emptyList(),
    val nativeLibrary: List<NativeLibraryPluginUi> = emptyList(),
    val nativeRuntimeKnown: Boolean = false,
    val nativePluginActionPath: String? = null,
    val directoryPlugins: List<DirectoryPluginUi> = emptyList(),
    val pluginDirectoryLoading: Boolean = false,
    val pluginDirectoryError: String? = null,
    val directoryRuntimeError: String? = null,
    val directoryTargetOffline: Boolean = false,
    val pluginImporting: Boolean = false,
    val pluginActionId: String? = null,
    val packageEvents: PackageEventsUi? = null,
    val pluginListenPorts: Map<Pair<String, String>, Int> = emptyMap(),
    val installedApps: List<InstalledApp> = emptyList(),
    val appsLoading: Boolean = false,
    val logText: String = "",
    val logcatRunning: Boolean = false,
    val lastMessage: String? = null,
    val lastError: Boolean = false,
)

internal fun ConsoleUiState.canOperatePlugin(id: String, target: String): Boolean =
    activePluginId == id && selectedTarget == target && target in pluginScopes[id].orEmpty() &&
        scopeSavingId == null

internal fun ConsoleUiState.withSelectedTarget(target: String?): ConsoleUiState =
    if (selectedTarget == target) this else copy(
        selectedTarget = target,
        pluginsLoading = false,
        plugins = emptyList(),
        pluginsRaw = "",
        nativeLibrary = nativeLibrary.map { it.copy(runtime = null, runtimeKnown = false, error = null) },
        nativeRuntimeKnown = false,
        nativePluginActionPath = null,
        directoryPlugins = directoryPlugins.map {
            it.copy(runState = DirectoryPluginState.UNKNOWN, session = null, error = null)
        },
        pluginActionId = null,
        pluginDirectoryLoading = false,
        directoryRuntimeError = null,
        directoryTargetOffline = false,
        packageEvents = null,
    )
