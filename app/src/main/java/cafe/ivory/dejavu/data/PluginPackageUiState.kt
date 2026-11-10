package cafe.ivory.dejavu.data

enum class DirectoryPluginState { UNKNOWN, STOPPED, RUNNING, ERROR }

data class DirectoryPluginUi(
    val plugin: ImportedPluginPackage,
    val runState: DirectoryPluginState = DirectoryPluginState.UNKNOWN,
    val session: String? = null,
    val error: String? = null,
)

data class LivePluginEvent(
    val title: String,
    val content: String,
    val meta: String,
    val receivedAtMillis: Long? = null,
)

data class PackageEventsUi(
    val packageId: String,
    val packageName: String,
    val target: String,
    val connected: Boolean = false,
    val error: String? = null,
    val events: List<LivePluginEvent> = emptyList(),
    val listenPort: Int? = null,
    val session: String? = null,
) {
    fun matches(target: String?, id: String, session: String?): Boolean =
        session != null && this.target == target && packageId == id && this.session == session
}
