package cafe.ivory.dejavu.data

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.concurrent.atomic.AtomicLong

data class InstalledApp(
    val label: String,
    val packageName: String,
)

data class UiMessage(
    val id: Long,
    val text: String,
    val isError: Boolean = false,
)

data class EnvironmentUiState(
    val loading: Boolean = true,
    val rootGranted: Boolean = false,
    val helperRunning: Boolean = false,
    val helperPort: String = "",
    val targets: List<String> = emptyList(),
    val pluginScopes: Map<String, List<String>> = emptyMap(),
    val scopeSavingId: String? = null,
    val manualRemovingTarget: String? = null,
    val installedApps: List<InstalledApp> = emptyList(),
    val appsLoading: Boolean = false,
    val message: UiMessage? = null,
)

internal data class EnvironmentStrings(
    val scopeSaved: () -> String = { "Saved" },
    val scopeWriteError: (String) -> String = { "Error: $it" },
    val manualTargetRemoved: (String) -> String = { "Removed $it" },
)

private val uiMessageIds = AtomicLong(1L)

internal class EnvironmentViewModel(
    private val shell: ShellExecutor = RootShell,
    private val scopeStore: PluginScopeStore = PluginScopeStore(shell),
    private val loadAppsProvider: suspend () -> List<InstalledApp> = { emptyList() },
    private val strings: EnvironmentStrings = EnvironmentStrings(),
    private val coroutineScope: CoroutineScope? = null,
) : ViewModel() {
    private val scope get() = coroutineScope ?: viewModelScope
    private val _state = MutableStateFlow(EnvironmentUiState())
    val state: StateFlow<EnvironmentUiState> = _state.asStateFlow()

    private var refreshJob: Job? = null
    private var appsLoadJob: Job? = null
    private var appsLoaded = false
    private val scopeEpoch = AtomicLong(0L)

    fun refresh() {
        val current = _state.value
        if (refreshJob?.isActive == true || current.scopeSavingId != null || current.manualRemovingTarget != null) return
        val epochAtStart = scopeEpoch.get()
        refreshJob = scope.launch {
            _state.update { it.copy(loading = true) }
            try {
                val root = withContext(Dispatchers.IO) { shell.isRoot() }
                if (!root) {
                    _state.update {
                        it.copy(
                            loading = false,
                            rootGranted = false,
                            helperRunning = false,
                            helperPort = "",
                        )
                    }
                    return@launch
                }
                val config = withContext(Dispatchers.IO) { runCatching { scopeStore.read() } }
                val helperPort = withContext(Dispatchers.IO) { shell.getProp("debug.dejavu.root.port") }
                val helperToken = withContext(Dispatchers.IO) { shell.getProp("debug.dejavu.root.token") }
                val helperReady = helperPort.matches(Regex("\\d{1,5}")) &&
                    helperToken.matches(Regex("[0-9a-f]{64}"))

                // Do not overwrite scopes if a save/remove mutation started while refresh was in flight.
                if (scopeEpoch.get() == epochAtStart && _state.value.scopeSavingId == null && _state.value.manualRemovingTarget == null) {
                    config.fold(
                        onSuccess = { snapshot ->
                            _state.update { it.copy(targets = snapshot.targets, pluginScopes = snapshot.scopes) }
                        },
                        onFailure = { error ->
                            if (error.message?.contains("No such file", ignoreCase = true) != true) {
                                postMessage(strings.scopeWriteError(error.message.orEmpty()), error = true)
                            }
                        },
                    )
                }
                _state.update {
                    it.copy(
                        loading = false,
                        rootGranted = true,
                        helperRunning = helperReady,
                        helperPort = helperPort,
                    )
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (_: Exception) {
                _state.update { it.copy(loading = false) }
            }
        }
    }

    fun loadInstalledApps(force: Boolean = false) {
        if (!force && appsLoaded) return
        if (appsLoadJob?.isActive == true) return
        appsLoadJob = scope.launch {
            _state.update { it.copy(appsLoading = true) }
            try {
                val apps = withContext(Dispatchers.IO) { loadAppsProvider() }
                appsLoaded = true
                _state.update { it.copy(installedApps = apps) }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (_: Exception) {
                // Keep previously loaded apps instead of crashing on PackageManager IPC errors.
            } finally {
                _state.update { it.copy(appsLoading = false) }
                appsLoadJob = null
            }
        }
    }

    fun savePluginScope(
        id: String,
        targets: List<String>,
        onCompleted: (PluginScopesSnapshot?) -> Unit = {},
    ) {
        if (_state.value.scopeSavingId != null || _state.value.manualRemovingTarget != null) return
        scopeEpoch.incrementAndGet()
        _state.update { it.copy(scopeSavingId = id) }
        scope.launch {
            var saved: PluginScopesSnapshot? = null
            try {
                val snapshot = withContext(Dispatchers.IO) { scopeStore.setScope(id, targets) }
                saved = snapshot
                scopeEpoch.incrementAndGet()
                _state.update { it.copy(targets = snapshot.targets, pluginScopes = snapshot.scopes) }
                postMessage(strings.scopeSaved())
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                postMessage(strings.scopeWriteError(error.message.orEmpty()), error = true)
            } finally {
                _state.update { it.copy(scopeSavingId = null) }
                onCompleted(saved)
            }
        }
    }

    fun removeManualTarget(process: String) {
        if (_state.value.scopeSavingId != null || _state.value.manualRemovingTarget != null) return
        scopeEpoch.incrementAndGet()
        _state.update { it.copy(manualRemovingTarget = process) }
        scope.launch {
            try {
                val snapshot = withContext(Dispatchers.IO) { scopeStore.removeManualTarget(process) }
                scopeEpoch.incrementAndGet()
                _state.update { it.copy(targets = snapshot.targets, pluginScopes = snapshot.scopes) }
                postMessage(strings.manualTargetRemoved(process))
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                postMessage(strings.scopeWriteError(error.message.orEmpty()), error = true)
            } finally {
                _state.update { it.copy(manualRemovingTarget = null) }
            }
        }
    }

    fun postMessage(text: String, error: Boolean = false) {
        _state.update { it.copy(message = UiMessage(uiMessageIds.getAndIncrement(), text, error)) }
    }

    fun consumeMessage(id: Long) {
        _state.update { if (it.message?.id == id) it.copy(message = null) else it }
    }
}
