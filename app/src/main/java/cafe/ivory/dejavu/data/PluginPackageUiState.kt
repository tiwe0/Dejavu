package cafe.ivory.dejavu.data

import java.util.concurrent.atomic.AtomicLong

enum class DirectoryPluginState { UNKNOWN, STOPPED, RUNNING, ERROR }

data class DirectoryPluginUi(
    val plugin: ImportedPluginPackage,
    val runState: DirectoryPluginState = DirectoryPluginState.UNKNOWN,
    val session: String? = null,
    val error: String? = null,
)

private val liveEventIds = AtomicLong(1L)

internal fun nextLiveEventId(): Long = liveEventIds.getAndIncrement()

data class LivePluginEvent(
    val title: String,
    val content: String,
    val meta: String,
    val receivedAtMillis: Long? = null,
) {
    /** Monotonic per-instance identity for Compose LazyColumn keys without affecting value equality. */
    val id: Long = nextLiveEventId()
}

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

/** Unified presentation item shared by Overview, Plugin list, TopBar and Info views. */
internal sealed interface LibraryPluginItem {
    val id: String
    val name: String
    val version: String
    val description: String
    val kindTag: String

    data class Lua(val ui: DirectoryPluginUi) : LibraryPluginItem {
        override val id get() = ui.plugin.manifest.id
        override val name get() = ui.plugin.manifest.name
        override val version get() = ui.plugin.manifest.version
        override val description get() = ui.plugin.manifest.description
        override val kindTag get() = "LUA"
    }

    data class Native(val ui: NativeLibraryPluginUi) : LibraryPluginItem {
        override val id get() = ui.artifact.id.orEmpty()
        override val name get() = ui.artifact.name
        override val version get() = ui.artifact.version
        override val description get() = ui.artifact.description
        override val kindTag get() = if (ui.artifact.jit) "JIT" else "NATIVE"
    }
}
