package cafe.ivory.dejavu.data

import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

data class LogsUiState(
    val logText: String = "",
    val logVersion: Long = 0L,
    val running: Boolean = false,
)

private const val LOG_TAGS =
    "DejavuZygisk:V DejavuAgent:V DejavuPlugin:V DejavuHook:V DejavuNative:V"
private const val MAX_LOG_CHARS = 80_000
private const val LOG_FLUSH_INTERVAL_MS = 120L

/**
 * Isolated logcat stream ViewModel so high-frequency log flushes only recompose [LogsScreen].
 * Suspends the root logcat process when the screen is not visible/resumed, and uses a trailing
 * flush timer so short bursts never remain stranded in the buffer.
 */
internal class LogsViewModel(
    private val shell: ShellExecutor = RootShell,
    private val excludedMarkersProvider: () -> Set<String> = { emptySet() },
    private val onError: (String) -> Unit = {},
    private val coroutineScope: CoroutineScope? = null,
    private val flushIntervalMs: Long = LOG_FLUSH_INTERVAL_MS,
) : ViewModel() {
    private val scope get() = coroutineScope ?: viewModelScope
    private val _state = MutableStateFlow(LogsUiState())
    val state: StateFlow<LogsUiState> = _state.asStateFlow()

    private val lock = Any()
    private val builder = StringBuilder()
    private var logProcess: Process? = null
    private var logJob: Job? = null
    private var flushJob: Job? = null
    private var serial = 0
    private var dirty = false
    private var visible = false
    private var userStopped = false

    fun setVisible(isVisible: Boolean) {
        if (visible == isVisible) return
        visible = isVisible
        if (isVisible) {
            if (!userStopped) startInternal(resetBuffer = false)
        } else {
            stopInternal(markUserStopped = false)
        }
    }

    fun start() {
        userStopped = false
        startInternal(resetBuffer = true)
    }

    fun stop() {
        stopInternal(markUserStopped = true)
    }

    fun clear() {
        synchronized(lock) {
            builder.setLength(0)
            dirty = false
            _state.update { it.copy(logText = "", logVersion = it.logVersion + 1) }
        }
    }

    private fun startInternal(resetBuffer: Boolean) {
        if (logJob?.isActive == true) return
        val runSerial = synchronized(lock) {
            if (resetBuffer) {
                builder.setLength(0)
                dirty = false
                _state.update { it.copy(logText = "", logVersion = it.logVersion + 1, running = true) }
            } else {
                _state.update { it.copy(running = true) }
            }
            ++serial
        }
        flushJob?.cancel()
        flushJob = scope.launch(Dispatchers.Default) {
            while (isActive) {
                delay(flushIntervalMs)
                flushIfDirty(runSerial)
            }
        }
        logJob = scope.launch(Dispatchers.IO) {
            var process: Process? = null
            try {
                process = shell.startLogcat("-v time -s $LOG_TAGS")
                synchronized(lock) {
                    if (runSerial != serial || !isActive) return@launch
                    logProcess = process
                }
                process.inputStream.bufferedReader().useLines { lines ->
                    lines.forEach { line ->
                        // Package events have their own clear-on-close viewer;
                        // keep message frames out of the general log buffer.
                        val markers = excludedMarkersProvider()
                        if (markers.any { marker -> line.contains("$marker ") }) return@forEach
                        synchronized(lock) {
                            if (runSerial != serial || !isActive) return@launch
                            builder.appendLine(line)
                            if (builder.length > MAX_LOG_CHARS) {
                                builder.delete(0, builder.length - MAX_LOG_CHARS)
                            }
                            dirty = true
                        }
                    }
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                val shouldReport = synchronized(lock) { runSerial == serial && process == null }
                if (shouldReport) onError(error.message.orEmpty())
            } finally {
                shell.stopLogcat(process)
                synchronized(lock) {
                    if (runSerial == serial) {
                        logProcess = null
                        dirty = false
                        val snapshot = builder.toString()
                        _state.update {
                            it.copy(
                                running = false,
                                logText = snapshot,
                                logVersion = it.logVersion + 1,
                            )
                        }
                    }
                }
            }
        }
    }

    private fun flushIfDirty(expectedSerial: Int) {
        val snapshot = synchronized(lock) {
            if (expectedSerial != serial || !dirty) return
            dirty = false
            builder.toString()
        }
        _state.update { it.copy(logText = snapshot, logVersion = it.logVersion + 1) }
    }

    private fun stopInternal(markUserStopped: Boolean) {
        if (markUserStopped) userStopped = true
        if (logJob == null && flushJob == null) return
        val process = synchronized(lock) {
            ++serial
            dirty = false
            val snapshot = builder.toString()
            _state.update { it.copy(running = false, logText = snapshot, logVersion = it.logVersion + 1) }
            logProcess.also { logProcess = null }
        }
        flushJob?.cancel()
        flushJob = null
        logJob?.cancel()
        logJob = null
        shell.stopLogcat(process)
    }

    override fun onCleared() {
        stopInternal(markUserStopped = false)
        super.onCleared()
    }

    companion object {
        fun factory(
            excludedMarkersProvider: () -> Set<String>,
            onError: (String) -> Unit,
        ): ViewModelProvider.Factory = object : ViewModelProvider.Factory {
            @Suppress("UNCHECKED_CAST")
            override fun <T : ViewModel> create(modelClass: Class<T>): T =
                LogsViewModel(
                    shell = RootShell,
                    excludedMarkersProvider = excludedMarkersProvider,
                    onError = onError,
                ) as T
        }
    }
}
