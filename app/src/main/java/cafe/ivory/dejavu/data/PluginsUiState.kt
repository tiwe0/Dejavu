package cafe.ivory.dejavu.data

data class PluginInfo(
    val name: String,
    val version: String,
    val loaded: Boolean,
    val jit: Boolean,
    val skill: Boolean,
    val path: String,
)

data class NativeLibraryPluginUi(
    val artifact: NativePluginArtifact,
    val runtime: PluginInfo? = null,
    val runtimeKnown: Boolean = false,
    val error: String? = null,
)

data class SkillSheetState(
    val loading: Boolean = true,
    val text: String = "",
)

data class PluginsUiState(
    val pluginsLoading: Boolean = false,
    val selectedTarget: String? = null,
    val activePluginId: String? = null,
    val plugins: List<PluginInfo> = emptyList(),
    val nativeLibrary: List<NativeLibraryPluginUi> = emptyList(),
    val nativeRuntimeKnown: Boolean = false,
    val nativePluginActionPath: String? = null,
    val directoryPlugins: List<DirectoryPluginUi> = emptyList(),
    val directoryRuntimeSnapshot: Map<String, PackageRuntimeSnapshot>? = null,
    val pluginDirectoryLoading: Boolean = false,
    val pluginDirectoryError: String? = null,
    val directoryRuntimeError: String? = null,
    val directoryTargetOffline: Boolean = false,
    val pluginImporting: Boolean = false,
    val pluginActionId: String? = null,
    val packageEvents: PackageEventsUi? = null,
    val pluginListenPorts: Map<Pair<String, String>, Int> = emptyMap(),
    val skillSheet: SkillSheetState? = null,
) {
    internal val allLibraryItems: List<LibraryPluginItem>
        get() = directoryPlugins.map { LibraryPluginItem.Lua(it) } +
            nativeLibrary.mapNotNull { item -> item.artifact.id?.let { LibraryPluginItem.Native(item) } }

    internal fun findItem(id: String): LibraryPluginItem? =
        directoryPlugins.firstOrNull { it.plugin.manifest.id == id }?.let { LibraryPluginItem.Lua(it) }
            ?: nativeLibrary.firstOrNull { it.artifact.id == id }?.let { LibraryPluginItem.Native(it) }
}

internal fun PluginsUiState.operationInProgress(scopeSavingId: String? = null): Boolean =
    pluginImporting || pluginActionId != null || nativePluginActionPath != null || scopeSavingId != null

internal fun PluginsUiState.withDirectoryPackages(packages: List<ImportedPluginPackage>): PluginsUiState {
    val previous = directoryPlugins.associateBy { it.plugin.manifest.id }
    return copy(directoryPlugins = packages.map { plugin ->
        val status = directoryRuntimeSnapshot?.get(plugin.manifest.id)
        DirectoryPluginUi(
            plugin = plugin,
            runState = when {
                selectedTarget == null || directoryRuntimeSnapshot == null -> DirectoryPluginState.UNKNOWN
                status != null -> status.state
                else -> DirectoryPluginState.STOPPED
            },
            session = status?.session,
            error = previous[plugin.manifest.id]?.takeIf {
                status?.state == DirectoryPluginState.ERROR && it.session == status.session
            }?.error,
        )
    })
}

internal fun PluginsUiState.canOperatePlugin(
    id: String,
    target: String,
    pluginScopes: Map<String, List<String>>,
    scopeSavingId: String? = null,
): Boolean = activePluginId == id && selectedTarget == target && target in pluginScopes[id].orEmpty() &&
    scopeSavingId == null

internal fun PluginsUiState.withSelectedTarget(target: String?): PluginsUiState =
    if (selectedTarget == target) this else copy(
        selectedTarget = target,
        pluginsLoading = false,
        plugins = emptyList(),
        nativeLibrary = nativeLibrary.map { it.copy(runtime = null, runtimeKnown = false, error = null) },
        nativeRuntimeKnown = false,
        nativePluginActionPath = null,
        directoryPlugins = directoryPlugins.map {
            it.copy(runState = DirectoryPluginState.UNKNOWN, session = null, error = null)
        },
        directoryRuntimeSnapshot = null,
        pluginActionId = null,
        directoryRuntimeError = null,
        directoryTargetOffline = false,
        packageEvents = null,
        skillSheet = null,
    )
