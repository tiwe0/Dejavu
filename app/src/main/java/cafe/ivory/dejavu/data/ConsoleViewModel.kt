package cafe.ivory.dejavu.data

import android.app.Application
import android.content.Context
import android.content.Intent
import androidx.annotation.StringRes
import cafe.ivory.dejavu.R
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.BufferedReader
import java.io.InputStreamReader

private const val MODULE_DIR = "/data/adb/modules/dejavu_zygisk"
private const val TARGETS_FILE = "$MODULE_DIR/config/targets.txt"
private const val PREFERENCES = "dejavu-console"
private const val SELECTED_TARGET = "selected-target"
private const val LOG_TAGS =
    "DejavuZygisk:V DejavuAgent:V DejavuPlugin:V DejavuHook:V DejavuNative:V"

data class InstalledApp(
    val label: String,
    val packageName: String,
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
    val pluginsRaw: String = "",
    val plugins: List<PluginInfo> = emptyList(),
    val installedApps: List<InstalledApp> = emptyList(),
    val appsLoading: Boolean = false,
    val logText: String = "",
    val logcatRunning: Boolean = false,
    val lastMessage: String? = null,
    val lastError: Boolean = false,
)

class ConsoleViewModel(application: Application) : AndroidViewModel(application) {
    private val preferences = application.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
    private val _state = MutableStateFlow(ConsoleUiState())
    val state: StateFlow<ConsoleUiState> = _state.asStateFlow()

    private var refreshJob: Job? = null
    private var pluginJob: Job? = null
    private var pluginRequestSerial = 0
    private var appsLoaded = false
    private var logProcess: Process? = null
    private var logJob: Job? = null
    private val logBuilder = StringBuilder()

    init {
        refresh()
    }

    fun refresh() {
        if (refreshJob?.isActive == true) return
        refreshJob = viewModelScope.launch {
            _state.update { it.copy(loading = true) }
            val root = withContext(Dispatchers.IO) { RootShell.isRoot() }
            if (!root) {
                _state.update {
                    it.copy(
                        loading = false,
                        rootGranted = false,
                        moduleInstalled = false,
                        moduleVersion = "",
                        helperRunning = false,
                        helperPort = "",
                        helperPid = "",
                        targets = emptyList(),
                        selectedTarget = null,
                        plugins = emptyList(),
                        pluginsRaw = "",
                    )
                }
                return@launch
            }
            val prop = withContext(Dispatchers.IO) { RootShell.readFile("$MODULE_DIR/module.prop") }
            val version = prop?.lineSequence()
                ?.firstOrNull { it.startsWith("version=") }
                ?.substringAfter("=")
                ?.trim()
                .orEmpty()
            val targets = withContext(Dispatchers.IO) { readTargets() }
            val helperPort = if (root) withContext(Dispatchers.IO) {
                RootShell.getProp("debug.dejavu.root.port")
            } else ""
            val helperToken = if (root) withContext(Dispatchers.IO) {
                RootShell.getProp("debug.dejavu.root.token")
            } else ""
            val helperPid = if (root) withContext(Dispatchers.IO) {
                RootShell.run("pidof dejavu-root-helper").output.trim()
            } else ""
            val helperReady = helperPort.matches(Regex("\\d{1,5}")) &&
                helperToken.matches(Regex("[0-9a-f]{64}"))
            val saved = preferences.getString(SELECTED_TARGET, null)
            val selected = _state.value.selectedTarget
                ?.takeIf { it in targets }
                ?: saved?.takeIf { it in targets }
                ?: targets.firstOrNull()
            if (selected != null) {
                preferences.edit().putString(SELECTED_TARGET, selected).apply()
            } else {
                preferences.edit().remove(SELECTED_TARGET).apply()
            }
            _state.update {
                it.copy(
                    loading = false,
                    rootGranted = root,
                    moduleInstalled = prop != null,
                    moduleVersion = version,
                    helperRunning = helperReady,
                    helperPort = helperPort,
                    helperPid = helperPid,
                    targets = targets,
                    selectedTarget = selected,
                    plugins = if (it.selectedTarget == selected) it.plugins else emptyList(),
                    pluginsRaw = if (it.selectedTarget == selected) it.pluginsRaw else "",
                )
            }
        }
    }

    fun loadInstalledApps(force: Boolean = false) {
        if (appsLoaded && !force) return
        viewModelScope.launch {
            _state.update { it.copy(appsLoading = true) }
            val apps = withContext(Dispatchers.IO) {
                val app = getApplication<Application>()
                val manager = app.packageManager
                val intent = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
                manager.queryIntentActivities(intent, 0)
                    .asSequence()
                    .mapNotNull { info ->
                        val packageName = info.activityInfo?.packageName ?: return@mapNotNull null
                        if (packageName == app.packageName) return@mapNotNull null
                        InstalledApp(
                            label = info.loadLabel(manager).toString().ifBlank { packageName },
                            packageName = packageName,
                        )
                    }
                    .distinctBy { it.packageName }
                    .sortedBy { it.label.lowercase() }
                    .toList()
            }
            appsLoaded = true
            _state.update { it.copy(installedApps = apps, appsLoading = false) }
        }
    }

    fun selectTarget(process: String) {
        if (_state.value.selectedTarget == process) return
        preferences.edit().putString(SELECTED_TARGET, process).apply()
        _state.update {
            it.copy(
                selectedTarget = process,
                plugins = emptyList(),
                pluginsRaw = "",
            )
        }
    }

    fun addTarget(process: String) {
        val name = process.trim()
        if (!name.matches(Regex("[A-Za-z0-9_.:-]{1,128}"))) {
            message(text(R.string.msg_invalid_process), error = true)
            return
        }
        viewModelScope.launch {
            _state.update { it.copy(loading = true) }
            val targets = withContext(Dispatchers.IO) { (readTargets() + name).distinct() }
            val ok = withContext(Dispatchers.IO) { writeTargets(targets) }
            _state.update { it.copy(loading = false) }
            if (ok) {
                preferences.edit().putString(SELECTED_TARGET, name).apply()
                _state.update { it.copy(targets = targets, selectedTarget = name) }
                message(text(R.string.msg_added_target, name))
            } else {
                message(text(R.string.msg_unable_write_targets), error = true)
            }
        }
    }

    fun removeTarget(process: String) {
        viewModelScope.launch {
            _state.update { it.copy(loading = true) }
            val targets = withContext(Dispatchers.IO) { readTargets().filterNot { it == process } }
            val ok = withContext(Dispatchers.IO) { writeTargets(targets) }
            if (ok) {
                val next = _state.value.selectedTarget
                    ?.takeIf { it in targets }
                    ?: targets.firstOrNull()
                if (next != null) {
                    preferences.edit().putString(SELECTED_TARGET, next).apply()
                } else {
                    preferences.edit().remove(SELECTED_TARGET).apply()
                }
                _state.update {
                    it.copy(
                        loading = false,
                        targets = targets,
                        selectedTarget = next,
                        plugins = emptyList(),
                        pluginsRaw = "",
                    )
                }
                message(text(R.string.msg_removed_target, process))
            } else {
                _state.update { it.copy(loading = false) }
                message(text(R.string.msg_unable_write_targets), error = true)
            }
        }
    }

    fun restartTarget(process: String) {
        viewModelScope.launch {
            _state.update { it.copy(loading = true) }
            val result = withContext(Dispatchers.IO) {
                val stop = RootShell.run("am force-stop ${RootShell.quote(process)}")
                val start = RootShell.run(
                    "monkey -p ${RootShell.quote(process)} -c android.intent.category.LAUNCHER 1",
                )
                stop to start
            }
            _state.update { it.copy(loading = false) }
            if (result.second.exitCode == 0) {
                message(text(R.string.msg_restarted_target, process))
            } else {
                message(
                    text(
                        R.string.msg_restart_failed,
                        (result.first.output + result.second.output).trim().take(160),
                    ),
                    error = true,
                )
            }
        }
    }

    fun refreshPlugins() {
        val target = _state.value.selectedTarget ?: run {
            message(text(R.string.msg_select_target), error = true)
            return
        }
        val serial = ++pluginRequestSerial
        pluginJob?.cancel()
        pluginJob = viewModelScope.launch {
            _state.update { it.copy(pluginsLoading = true) }
            try {
                val raw = withContext(Dispatchers.IO) { DejavuRpc.listPluginStatus(target) }
                if (serial != pluginRequestSerial || _state.value.selectedTarget != target) return@launch
                _state.update {
                    it.copy(
                        pluginsLoading = false,
                        pluginsRaw = raw,
                        plugins = parsePlugins(raw),
                    )
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                if (serial != pluginRequestSerial) return@launch
                _state.update {
                    it.copy(pluginsLoading = false, pluginsRaw = "", plugins = emptyList())
                }
                message(text(R.string.msg_plugin_list_failed, error.message ?: ""), error = true)
            }
        }
    }

    fun unloadPlugin(name: String) {
        val target = _state.value.selectedTarget ?: return
        viewModelScope.launch {
            _state.update { it.copy(loading = true) }
            val result = withContext(Dispatchers.IO) {
                runCatching { DejavuRpc.unloadPlugin(target, name) }
            }
            _state.update { it.copy(loading = false) }
            result.fold(
                onSuccess = {
                    message(it)
                    refreshPlugins()
                },
                onFailure = {
                    message(text(R.string.msg_unload_failed, it.message ?: ""), error = true)
                },
            )
        }
    }

    suspend fun pluginSkill(name: String): Result<String> {
        val target = _state.value.selectedTarget
            ?: return Result.failure(RpcException("Select a target first"))
        return withContext(Dispatchers.IO) { runCatching { DejavuRpc.pluginSkill(target, name) } }
    }

    fun loadPlugin(path: String) {
        val target = _state.value.selectedTarget ?: run {
            message(text(R.string.msg_select_target), error = true)
            return
        }
        val trimmed = path.trim()
        if (!trimmed.startsWith("/") || !trimmed.endsWith(".so")) {
            message(text(R.string.msg_invalid_plugin_path), error = true)
            return
        }
        viewModelScope.launch {
            _state.update { it.copy(loading = true) }
            val result = withContext(Dispatchers.IO) {
                runCatching { DejavuRpc.loadPlugin(target, trimmed) }
            }
            _state.update { it.copy(loading = false) }
            result.fold(
                onSuccess = {
                    message(it)
                    refreshPlugins()
                },
                onFailure = {
                    message(text(R.string.msg_load_failed, it.message ?: ""), error = true)
                },
            )
        }
    }

    fun callPlugin(plugin: String, function: String, vararg args: String) {
        val target = _state.value.selectedTarget ?: run {
            message(text(R.string.msg_select_target), error = true)
            return
        }
        viewModelScope.launch {
            _state.update { it.copy(loading = true) }
            val result = withContext(Dispatchers.IO) {
                runCatching { DejavuRpc.callPlugin(target, plugin, function, *args) }
            }
            _state.update { it.copy(loading = false) }
            result.fold(
                onSuccess = { message(it.take(600)) },
                onFailure = {
                    message(text(R.string.msg_plugin_call_failed, it.message ?: ""), error = true)
                },
            )
        }
    }

    suspend fun runScript(script: String): Result<String> {
        val target = _state.value.selectedTarget
            ?: return Result.failure(RpcException("Select a target first"))
        _state.update { it.copy(loading = true) }
        return withContext(Dispatchers.IO) {
            runCatching { DejavuRpc.execute(target, script) }
        }.also { _state.update { it.copy(loading = false) } }
    }

    fun startLogs() {
        if (logJob?.isActive == true) return
        logBuilder.setLength(0)
        _state.update { it.copy(logText = "", logcatRunning = true) }
        logJob = viewModelScope.launch(Dispatchers.IO) {
            val process = try {
                ProcessBuilder("/system/bin/su", "-c", "logcat -v time -s $LOG_TAGS")
                    .redirectErrorStream(true)
                    .start()
            } catch (error: Exception) {
                withContext(Dispatchers.Main) {
                    _state.update { it.copy(logcatRunning = false) }
                    message(text(R.string.msg_logcat_failed, error.message ?: ""), error = true)
                }
                return@launch
            }
            logProcess = process
            var lastEmit = 0L
            try {
                BufferedReader(InputStreamReader(process.inputStream)).useLines { lines ->
                    lines.forEach { line ->
                        synchronized(logBuilder) {
                            logBuilder.appendLine(line)
                            if (logBuilder.length > 80_000) {
                                logBuilder.delete(0, logBuilder.length - 80_000)
                            }
                        }
                        val now = System.currentTimeMillis()
                        if (now - lastEmit >= 120) {
                            lastEmit = now
                            val snapshot = synchronized(logBuilder) { logBuilder.toString() }
                            withContext(Dispatchers.Main) {
                                _state.update { it.copy(logText = snapshot) }
                            }
                        }
                    }
                }
            } catch (_: Exception) {
            } finally {
                val snapshot = synchronized(logBuilder) { logBuilder.toString() }
                withContext(Dispatchers.Main) {
                    _state.update { it.copy(logcatRunning = false, logText = snapshot) }
                }
                logProcess = null
                logJob = null
            }
        }
    }

    fun stopLogs() {
        logProcess?.destroy()
        logProcess = null
        logJob?.cancel()
        logJob = null
        _state.update { it.copy(logcatRunning = false) }
    }

    fun clearLogs() {
        logBuilder.setLength(0)
        _state.update { it.copy(logText = "") }
    }

    fun message(text: String, error: Boolean = false) {
        _state.update { it.copy(lastMessage = text, lastError = error) }
    }

    private fun text(@StringRes id: Int, vararg args: Any): String =
        getApplication<Application>().getString(id, *args)

    fun consumeMessage() {
        _state.update { it.copy(lastMessage = null) }
    }

    override fun onCleared() {
        stopLogs()
        super.onCleared()
    }

    private fun readTargets(): List<String> {
        val raw = RootShell.readFile(TARGETS_FILE) ?: return emptyList()
        return raw.lineSequence()
            .map { it.trim() }
            .filter { it.isNotEmpty() && !it.startsWith("#") }
            .toList()
    }

    private fun writeTargets(targets: List<String>): Boolean {
        val content = buildString {
            appendLine("# One exact Android process name per line.")
            appendLine("# Managed by Dejavu Console.")
            targets.forEach { appendLine(it) }
        }
        return RootShell.writeFile(TARGETS_FILE, content)
    }

    private fun parsePlugins(raw: String): List<PluginInfo> {
        val regex = Regex(
            """\[([A-Za-z0-9_.-]+)(?: ([^ ]+))? loaded=(yes|no) jit=(yes|no) caps=(0x[0-9a-fA-F]+) skill=(yes|no) path=(.*?)\]""",
        )
        return regex.findAll(raw).map { match ->
            PluginInfo(
                name = match.groupValues[1],
                version = match.groupValues[2],
                loaded = match.groupValues[3] == "yes",
                jit = match.groupValues[4] == "yes",
                capabilities = match.groupValues[5],
                skill = match.groupValues[6] == "yes",
                path = match.groupValues[7],
            )
        }.toList()
    }
}
