package cafe.ivory.dejavu.data

enum class DirectoryPluginState { UNKNOWN, STOPPED, RUNNING, ERROR }

data class DirectoryPluginUi(
    val plugin: ImportedPluginPackage,
    val runState: DirectoryPluginState = DirectoryPluginState.UNKNOWN,
    val session: String? = null,
    val error: String? = null,
)

data class LivePluginEvent(val title: String, val content: String, val meta: String)

data class PackageEventsUi(
    val packageId: String,
    val packageName: String,
    val target: String,
    val connected: Boolean = false,
    val error: String? = null,
    val events: List<LivePluginEvent> = emptyList(),
)
