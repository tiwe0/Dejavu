package cafe.ivory.dejavu.data

internal enum class PluginDetailTab { Control, Scope, Info }

/** Single source of truth for Manager navigation destinations. */
internal sealed interface ManagerRoute {
    val isRoot: Boolean get() = false
    val depth: Int get() = if (isRoot) 0 else 1

    /**
     * Destination identity used by page-level transitions (`PredictiveBackContent` & `AnimatedContent`).
     * Switching tabs inside [PluginDetail] keeps the same [destinationKey] so only the inner tab transitions.
     */
    val destinationKey: String

    data object Overview : ManagerRoute {
        override val isRoot = true
        override val destinationKey = "overview"
    }

    data object Plugins : ManagerRoute {
        override val isRoot = true
        override val destinationKey = "plugins"
    }

    data object Configuration : ManagerRoute {
        override val isRoot = true
        override val destinationKey = "configuration"
    }

    data object Logs : ManagerRoute {
        override val destinationKey = "logs"
    }

    data object Router : ManagerRoute {
        override val destinationKey = "router"
    }

    data object Installer : ManagerRoute {
        override val destinationKey = "installer"
    }

    data class PluginDetail(
        val id: String,
        val tab: PluginDetailTab = PluginDetailTab.Control,
    ) : ManagerRoute {
        override val destinationKey get() = "plugin:$id"
    }
}

/** Pure JVM navigation stack with serialization for `rememberSaveable` and one-shot transition skipping. */
internal class ManagerBackStack(initial: List<ManagerRoute> = listOf(ManagerRoute.Overview)) {
    private val entries = initial.ifEmpty { listOf(ManagerRoute.Overview) }.toMutableList()
    private var skipNextPageTransition = false

    val stack: List<ManagerRoute> get() = entries.toList()
    val current: ManagerRoute get() = entries.last()
    val previous: ManagerRoute? get() = entries.getOrNull(entries.lastIndex - 1)
    val canPop: Boolean get() = entries.size > 1

    fun selectRoot(root: ManagerRoute) {
        require(root.isRoot) { "Expected a root destination, got $root" }
        skipNextPageTransition = false
        entries.clear()
        entries += root
    }

    fun push(route: ManagerRoute) {
        require(!route.isRoot) { "Use selectRoot for root destinations" }
        skipNextPageTransition = false
        if (entries.last().destinationKey == route.destinationKey) {
            entries[entries.lastIndex] = route
        } else {
            entries += route
        }
    }

    fun selectPluginTab(tab: PluginDetailTab) {
        val top = entries.last() as? ManagerRoute.PluginDetail ?: return
        if (top.tab != tab) entries[entries.lastIndex] = top.copy(tab = tab)
    }

    fun pop(fromGesture: Boolean = false): Boolean {
        if (entries.size <= 1) return false
        skipNextPageTransition = fromGesture
        entries.removeAt(entries.lastIndex)
        return true
    }

    /** Consumed once by the top-level page transition so a committed predictive-back gesture never sticks. */
    fun consumeSkipPageTransition(): Boolean {
        val value = skipNextPageTransition
        skipNextPageTransition = false
        return value
    }

    fun encode(): List<String> = entries.map { route ->
        when (route) {
            ManagerRoute.Overview -> "overview"
            ManagerRoute.Plugins -> "plugins"
            ManagerRoute.Configuration -> "configuration"
            ManagerRoute.Logs -> "logs"
            ManagerRoute.Router -> "router"
            ManagerRoute.Installer -> "installer"
            is ManagerRoute.PluginDetail -> "plugin:${route.tab.name}:${route.id}"
        }
    }

    companion object {
        fun decode(encoded: List<String>): ManagerBackStack {
            val routes = encoded.mapNotNull { token ->
                when {
                    token == "overview" -> ManagerRoute.Overview
                    token == "plugins" -> ManagerRoute.Plugins
                    token == "configuration" -> ManagerRoute.Configuration
                    token == "logs" -> ManagerRoute.Logs
                    token == "router" -> ManagerRoute.Router
                    token == "installer" -> ManagerRoute.Installer
                    token.startsWith("plugin:") -> {
                        val rest = token.removePrefix("plugin:")
                        val tabName = rest.substringBefore(':', "")
                        val id = rest.substringAfter(':', "")
                        val tab = PluginDetailTab.entries.firstOrNull { it.name == tabName } ?: PluginDetailTab.Control
                        if (id.isNotBlank()) ManagerRoute.PluginDetail(id, tab) else null
                    }
                    else -> null
                }
            }
            return ManagerBackStack(routes)
        }
    }
}
