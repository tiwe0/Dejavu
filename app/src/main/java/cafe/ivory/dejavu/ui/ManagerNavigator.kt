package cafe.ivory.dejavu.ui

import androidx.compose.runtime.Composable
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.Saver
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import cafe.ivory.dejavu.data.ManagerBackStack
import cafe.ivory.dejavu.data.ManagerRoute
import cafe.ivory.dejavu.data.PluginDetailTab

@Stable
internal class ManagerNavigator(private val backStack: ManagerBackStack = ManagerBackStack()) {
    var current by mutableStateOf(backStack.current)
        private set
    var previous by mutableStateOf(backStack.previous)
        private set
    var canPop by mutableStateOf(backStack.canPop)
        private set

    private fun sync() {
        current = backStack.current
        previous = backStack.previous
        canPop = backStack.canPop
    }

    fun selectRoot(root: ManagerRoute) {
        backStack.selectRoot(root)
        sync()
    }

    fun push(route: ManagerRoute) {
        backStack.push(route)
        sync()
    }

    fun selectPluginTab(tab: PluginDetailTab) {
        backStack.selectPluginTab(tab)
        sync()
    }

    fun pop(fromGesture: Boolean = false): Boolean {
        val popped = backStack.pop(fromGesture)
        if (popped) sync()
        return popped
    }

    fun consumeSkipPageTransition(): Boolean = backStack.consumeSkipPageTransition()

    fun encode(): List<String> = backStack.encode()

    companion object {
        val Saver: Saver<ManagerNavigator, List<String>> = Saver(
            save = { it.encode() },
            restore = { ManagerNavigator(ManagerBackStack.decode(it)) },
        )
    }
}

@Composable
internal fun rememberManagerNavigator(): ManagerNavigator =
    rememberSaveable(saver = ManagerNavigator.Saver) { ManagerNavigator() }
