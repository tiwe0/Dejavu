package cafe.ivory.dejavu.data

import android.app.Application
import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.core.graphics.drawable.toBitmap
import androidx.annotation.StringRes
import cafe.ivory.dejavu.R
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.BufferedReader
import java.io.InputStreamReader
import java.security.SecureRandom
import java.time.Instant
import org.json.JSONObject

private const val MODULE_DIR = "/data/adb/modules/dejavu_zygisk"
private const val TARGETS_FILE = "$MODULE_DIR/config/targets.txt"
private const val PREFERENCES = "dejavu-console"
private const val SELECTED_TARGET = "selected-target"
private const val LOG_TAGS =
    "DejavuZygisk:V DejavuAgent:V DejavuPlugin:V DejavuHook:V DejavuNative:V"

class ConsoleViewModel(application: Application) : AndroidViewModel(application) {
    private val preferences = application.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
    private val _state = MutableStateFlow(ConsoleUiState())
    val state: StateFlow<ConsoleUiState> = _state.asStateFlow()

    private var refreshJob: Job? = null
    private var pluginJob: Job? = null
    private var pluginRequestSerial = 0
    private var appsLoaded = false
    private var appsLoadJob: Job? = null
    private var logProcess: Process? = null
    private var logJob: Job? = null
    private val logBuilder = StringBuilder()
    private val packageImporter = PluginPackageImporter(application)
    private val nativeLibrary = NativePluginLibrary(application)
    private var nativeActionSerial = 0
    private var directoryJob: Job? = null
    private var directoryRequestSerial = 0
    private var packageActionSerial = 0
    private var packageEventJob: Job? = null
    private val packageEventLock = Any()
    private var packageEventProcess: Process? = null
    @Volatile
    private var packageEventSerial = 0

    init {
        refresh()
        loadInstalledApps()
    }

    fun refresh() {
        if (refreshJob?.isActive == true) return
        refreshJob = viewModelScope.launch {
            _state.update { it.copy(loading = true) }
            val root = withContext(Dispatchers.IO) { RootShell.isRoot() }
            if (!root) {
                updateSelectedTarget(null, emptyList(), persistSelection = false)
                _state.update {
                    it.copy(
                        loading = false,
                        rootGranted = false,
                        moduleInstalled = false,
                        moduleVersion = "",
                        helperRunning = false,
                        helperPort = "",
                        helperPid = "",
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
            updateSelectedTarget(selected, targets)
            _state.update {
                it.copy(
                    loading = false,
                    rootGranted = root,
                    moduleInstalled = prop != null,
                    moduleVersion = version,
                    helperRunning = helperReady,
                    helperPort = helperPort,
                    helperPid = helperPid,
                )
            }
        }
    }

    fun loadInstalledApps(force: Boolean = false) {
        if (!force && appsLoaded) return
        if (appsLoadJob?.isActive == true) return
        appsLoadJob = viewModelScope.launch {
            _state.update { it.copy(appsLoading = true) }
            try {
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
                                icon = runCatching {
                                    info.loadIcon(manager).toBitmap(96, 96)
                                }.getOrNull(),
                            )
                        }
                        .distinctBy { it.packageName }
                        .sortedBy { it.label.lowercase() }
                        .toList()
                }
                appsLoaded = true
                _state.update { it.copy(installedApps = apps, appsLoading = false) }
            } finally {
                appsLoadJob = null
            }
        }
    }

    fun selectTarget(process: String) {
        updateSelectedTarget(process)
    }

    private fun updateSelectedTarget(
        target: String?,
        targets: List<String> = _state.value.targets,
        persistSelection: Boolean = true,
    ) {
        if (_state.value.selectedTarget != target) {
            closePackageEvents()
            ++packageActionSerial
            ++nativeActionSerial
            ++directoryRequestSerial
            directoryJob?.cancel()
            ++pluginRequestSerial
            pluginJob?.cancel()
        }
        if (persistSelection) {
            if (target != null) {
                preferences.edit().putString(SELECTED_TARGET, target).apply()
            } else {
                preferences.edit().remove(SELECTED_TARGET).apply()
            }
        }
        _state.update { it.withSelectedTarget(target).copy(targets = targets) }
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
                updateSelectedTarget(name, targets)
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
                updateSelectedTarget(next, targets)
                _state.update { it.copy(loading = false) }
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
        refreshPluginLibrary()
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
                    bindNativeRuntime(it.copy(
                        pluginsLoading = false,
                        pluginsRaw = raw,
                        plugins = parsePlugins(raw),
                        nativeRuntimeKnown = true,
                    ))
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                if (serial != pluginRequestSerial) return@launch
                _state.update {
                    bindNativeRuntime(it.copy(pluginsLoading = false, pluginsRaw = "", plugins = emptyList(),
                        nativeRuntimeKnown = false))
                }
                message(text(R.string.msg_plugin_list_failed, error.message ?: ""), error = true)
            }
        }
    }

    fun unloadPlugin(target: String, name: String) {
        if (_state.value.selectedTarget != target) return
        viewModelScope.launch {
            _state.update { it.copy(loading = true) }
            val result = withContext(Dispatchers.IO) {
                runCatching { DejavuRpc.unloadPlugin(target, name) }
            }
            _state.update { it.copy(loading = false) }
            result.fold(
                onSuccess = {
                    message(it)
                    if (_state.value.selectedTarget == target) refreshPlugins()
                },
                onFailure = {
                    message(text(R.string.msg_unload_failed, it.message ?: ""), error = true)
                },
            )
        }
    }

    suspend fun pluginSkill(target: String, name: String): Result<String> {
        if (_state.value.selectedTarget != target) {
            return Result.failure(RpcException("Selected target changed"))
        }
        return withContext(Dispatchers.IO) { runCatching { DejavuRpc.pluginSkill(target, name) } }
    }

    fun loadPlugin(target: String, path: String) {
        if (_state.value.selectedTarget != target) return
        val trimmed = path.trim()
        if (!trimmed.startsWith("/") || (!trimmed.endsWith(".so") && !trimmed.endsWith(".c"))) {
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
                    if (_state.value.selectedTarget == target) refreshPlugins()
                },
                onFailure = {
                    message(text(R.string.msg_load_failed, it.message ?: ""), error = true)
                },
            )
        }
    }

    private fun bindNativeRuntime(state: ConsoleUiState): ConsoleUiState = state.copy(
        nativeLibrary = state.nativeLibrary.map { row ->
            val runtime = state.selectedTarget?.let { process ->
                state.plugins.firstOrNull { nativeLibrary.matchesDeployment(process, row.artifact, it.path) }
            }
            row.copy(
                runtimeKnown = state.selectedTarget != null && state.nativeRuntimeKnown,
                runtime = runtime,
                error = row.error.takeUnless { runtime?.loaded == true },
            )
        },
    )

    fun loadLibraryPlugin(target: String, path: String) {
        val current = _state.value
        if (current.selectedTarget != target || current.nativePluginActionPath != null ||
            current.pluginImporting || current.loading) return
        val artifact = current.nativeLibrary.firstOrNull { it.artifact.path == path }?.artifact ?: return
        val request = ++nativeActionSerial
        _state.update { state -> state.copy(nativePluginActionPath = path,
            nativeLibrary = state.nativeLibrary.map {
                if (it.artifact.path == path) it.copy(error = null) else it
            }) }
        viewModelScope.launch {
            try {
                val result = withContext(Dispatchers.IO) {
                    val deployed = nativeLibrary.deploy(target, artifact)
                    DejavuRpc.loadPlugin(target, deployed)
                }
                if (request == nativeActionSerial && _state.value.selectedTarget == target) message(result.take(300))
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                if (request == nativeActionSerial && _state.value.selectedTarget == target) {
                    val detail = text(R.string.msg_load_failed, error.message.orEmpty())
                    _state.update { state -> state.copy(nativeLibrary = state.nativeLibrary.map {
                        if (it.artifact.path == path) it.copy(error = detail) else it
                    }) }
                    message(detail, error = true)
                }
            } finally {
                if (request == nativeActionSerial && _state.value.selectedTarget == target) {
                    _state.update { it.copy(nativePluginActionPath = null) }
                    refreshPlugins()
                }
            }
        }
    }

    fun refreshPluginLibrary() {
        if (_state.value.pluginImporting || _state.value.pluginActionId != null ||
            _state.value.nativePluginActionPath != null) return
        val serial = ++directoryRequestSerial
        val target = _state.value.selectedTarget
        directoryJob?.cancel()
        directoryJob = viewModelScope.launch {
            _state.update { it.copy(pluginDirectoryLoading = true, pluginDirectoryError = null) }
            try {
                val (packages, artifacts, scanErrors) = withContext(Dispatchers.IO) {
                    val packages = packageImporter.list()
                    val artifacts = nativeLibrary.list()
                    Triple(packages, artifacts, packageImporter.lastScanErrors + nativeLibrary.lastScanErrors)
                }
                if (serial != directoryRequestSerial) return@launch
                _state.update { bindNativeRuntime(it.copy(
                    nativeLibrary = artifacts.map { artifact ->
                        NativeLibraryPluginUi(artifact, error = it.nativeLibrary
                            .firstOrNull { previous -> previous.artifact == artifact }?.error)
                    },
                )) }
                val runtime = if (target != null) withContext(Dispatchers.IO) {
                    runCatching { parsePackageRuntimeSnapshot(DejavuRpc.execute(target, LuaPackageScripts.snapshot())) }
                } else null
                if (serial != directoryRequestSerial) return@launch
                val sameTarget = _state.value.selectedTarget == target
                val previous = _state.value.directoryPlugins.associateBy { it.plugin.manifest.id }
                val rows = packages.map { plugin ->
                    val status = runtime?.getOrNull()?.get(plugin.manifest.id)
                    DirectoryPluginUi(
                        plugin = plugin,
                        runState = when {
                            !sameTarget || target == null || runtime?.isFailure == true -> DirectoryPluginState.UNKNOWN
                            status != null -> status.state
                            else -> DirectoryPluginState.STOPPED
                        },
                        session = status?.session.takeIf { sameTarget },
                        error = runtime?.exceptionOrNull()?.message.takeIf { sameTarget }
                            ?: previous[plugin.manifest.id]?.takeIf {
                                sameTarget && status?.state == DirectoryPluginState.ERROR && it.session == status.session
                            }?.error,
                    )
                }
                _state.update {
                    it.copy(
                        directoryPlugins = rows,
                        pluginDirectoryLoading = false,
                        pluginDirectoryError = scanErrors.takeIf { errors -> errors.isNotEmpty() }?.joinToString("\n"),
                    )
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                if (serial != directoryRequestSerial) return@launch
                _state.update {
                    it.copy(pluginDirectoryLoading = false, pluginDirectoryError =
                        text(R.string.package_scan_failed, error.message.orEmpty()))
                }
            }
        }
    }

    fun importPluginFolder(uri: Uri) {
        if (_state.value.pluginImporting || _state.value.pluginActionId != null ||
            _state.value.nativePluginActionPath != null) return
        viewModelScope.launch {
            _state.update { it.copy(pluginImporting = true) }
            try {
                val importedName = withContext(Dispatchers.IO) {
                    if (nativeLibrary.hasLuaManifest(uri)) {
                        packageImporter.importFolder(uri) { id ->
                            _state.value.directoryPlugins.none { row ->
                                row.plugin.manifest.id == id && row.runState != DirectoryPluginState.STOPPED
                            }
                        }
                            .manifest.name
                    } else nativeLibrary.importFolder(uri) { id ->
                        _state.value.directoryPlugins.none { row ->
                            row.plugin.manifest.id == id && row.runState != DirectoryPluginState.STOPPED
                        }
                    }.joinToString { it.name }
                }
                message(text(R.string.package_import_success, importedName))
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                message(text(R.string.package_import_failed, error.message.orEmpty()), error = true)
            } finally {
                _state.update { it.copy(pluginImporting = false) }
                refreshPluginLibrary()
            }
        }
    }

    fun startDirectoryPlugin(target: String, id: String) = runDirectoryPlugin(target, id, start = true)

    fun stopDirectoryPlugin(target: String, id: String) = runDirectoryPlugin(target, id, start = false)

    private fun runDirectoryPlugin(target: String, id: String, start: Boolean) {
        val current = _state.value
        if (current.selectedTarget != target || current.pluginActionId != null || current.pluginImporting ||
            current.nativePluginActionPath != null) return
        val row = current.directoryPlugins.firstOrNull { it.plugin.manifest.id == id } ?: return
        if (start && row.runState != DirectoryPluginState.STOPPED) return
        val session = if (start) ByteArray(8).also { SecureRandom().nextBytes(it) }
            .joinToString("") { "%02x".format(it.toInt() and 0xff) } else row.session
        val request = ++packageActionSerial
        ++directoryRequestSerial
        directoryJob?.cancel()
        _state.update { state ->
            state.copy(pluginActionId = id, pluginDirectoryLoading = false,
                directoryPlugins = state.directoryPlugins.map {
                    if (it.plugin.manifest.id == id) it.copy(error = null) else it
                })
        }
        viewModelScope.launch {
            try {
                val script = if (start) LuaPackageScripts.start(row.plugin, session!!)
                    else LuaPackageScripts.stop(id, session)
                val result = withContext(Dispatchers.IO) { DejavuRpc.execute(target, script) }
                if (request == packageActionSerial && _state.value.selectedTarget == target) {
                    message(result.take(300))
                    if (!start && _state.value.packageEvents?.packageId == id) closePackageEvents()
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                if (request == packageActionSerial && _state.value.selectedTarget == target) {
                    val detail = text(if (start) R.string.package_start_failed else R.string.package_stop_failed,
                        error.message.orEmpty())
                    _state.update { state ->
                        state.copy(directoryPlugins = state.directoryPlugins.map {
                            if (it.plugin.manifest.id == id) it.copy(
                                runState = DirectoryPluginState.ERROR, session = session, error = detail,
                            ) else it
                        })
                    }
                    message(detail, error = true)
                }
            } finally {
                if (request == packageActionSerial && _state.value.selectedTarget == target) {
                    _state.update { it.copy(pluginActionId = null) }
                    refreshPluginLibrary()
                }
            }
        }
    }

    fun openPackageEvents(target: String, id: String) {
        if (_state.value.selectedTarget != target) return
        val row = _state.value.directoryPlugins.firstOrNull { it.plugin.manifest.id == id } ?: return
        val spec = row.plugin.manifest.events ?: return
        val session = row.session ?: return
        if (row.runState != DirectoryPluginState.RUNNING) return
        closePackageEvents()
        val serial = packageEventSerial
        val openedAt = Instant.now()
        val since = "${openedAt.epochSecond}.${openedAt.nano.toString().padStart(9, '0')}"
        val decoder = PluginEventDecoder(spec.marker, session)
        _state.update { it.copy(packageEvents = PackageEventsUi(id, row.plugin.manifest.name, target)) }
        packageEventJob = viewModelScope.launch(Dispatchers.IO) {
            var process: Process? = null
            var expiryJob: Job? = null
            val seen = linkedMapOf<String, Long>()
            try {
                val pid = DejavuRpc.processPid(target)
                if (serial != packageEventSerial) return@launch
                process = RootShell.start("exec logcat --pid $pid -v raw -T $since -s ${RootShell.quote(spec.tag + ":I")} '*:S'")
                val accepted = synchronized(packageEventLock) {
                    if (serial != packageEventSerial) false else {
                        packageEventProcess = process
                        true
                    }
                }
                if (!accepted) return@launch
                _state.update { state ->
                    if (serial == packageEventSerial) state.copy(packageEvents = state.packageEvents?.copy(connected = true))
                    else state
                }
                expiryJob = launch(Dispatchers.Default) {
                    while (isActive) {
                        delay(1000)
                        decoder.feed("")
                    }
                }
                process.inputStream.bufferedReader().useLines { lines ->
                    lines.forEach { line ->
                        if (!isActive || serial != packageEventSerial) throw CancellationException("Event viewer closed")
                        val payload = decoder.feed(line) ?: return@forEach
                        val event = try { JSONObject(payload) } catch (_: Exception) { return@forEach }
                        val projected = projectPluginEvent(event, spec, row.plugin.manifest.name)
                        val now = android.os.SystemClock.elapsedRealtime()
                        seen.entries.removeAll { now - it.value > 30_000 }
                        val key = projected.dedupKey
                        if (key != null && seen.put(key, now) != null) return@forEach
                        if (seen.size > 4096) seen.remove(seen.keys.first())
                        val item = projected.item
                        _state.update { state ->
                            if (serial != packageEventSerial) state else state.copy(
                                packageEvents = state.packageEvents?.copy(
                                    events = (state.packageEvents.events + item).takeLast(50),
                                ),
                            )
                        }
                    }
                }
                if (serial == packageEventSerial) error("Live event stream closed")
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                if (serial == packageEventSerial) _state.update { state ->
                    state.copy(packageEvents = state.packageEvents?.copy(connected = false,
                        error = text(R.string.package_events_failed, error.message.orEmpty())))
                }
            } finally {
                expiryJob?.cancel()
                process?.destroy()
                decoder.clear()
                seen.clear()
                synchronized(packageEventLock) {
                    if (packageEventProcess === process) packageEventProcess = null
                }
            }
        }
    }

    fun closePackageEvents() {
        val process = synchronized(packageEventLock) {
            ++packageEventSerial
            packageEventProcess.also { packageEventProcess = null }
        }
        process?.destroy()
        packageEventJob?.cancel()
        packageEventJob = null
        _state.update { it.copy(packageEvents = null) }
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
                RootShell.start("logcat -v time -s $LOG_TAGS")
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
                        // Package events have their own clear-on-close viewer;
                        // keep message frames out of the general log buffer.
                        if (_state.value.directoryPlugins.any { item ->
                                item.plugin.manifest.events?.marker?.let { line.contains("$it ") } == true
                            }) return@forEach
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
        closePackageEvents()
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
