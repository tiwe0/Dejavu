package cafe.ivory.dejavu.data

import android.app.Application
import android.content.Intent
import android.net.Uri
import androidx.core.graphics.drawable.toBitmap
import androidx.annotation.StringRes
import cafe.ivory.dejavu.ManagerLanguage
import cafe.ivory.dejavu.R
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.isActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.security.SecureRandom
import java.time.Instant
import java.util.concurrent.ConcurrentHashMap
import org.json.JSONObject

private const val MODULE_DIR = "/data/adb/modules/dejavu_zygisk"
private const val LOG_TAGS =
    "DejavuZygisk:V DejavuAgent:V DejavuPlugin:V DejavuHook:V DejavuNative:V"

class ConsoleViewModel(application: Application) : AndroidViewModel(application) {
    private val scopeStore = PluginScopeStore()
    private val pluginTargets = mutableMapOf<String, String>()
    private val _state = MutableStateFlow(ConsoleUiState())
    val state: StateFlow<ConsoleUiState> = _state.asStateFlow()

    private var refreshJob: Job? = null
    private var pluginJob: Job? = null
    private var pluginCancellation: RpcCancellation? = null
    private var pluginRequestSerial = 0
    private var pluginRuntimeKey: Pair<String, Boolean>? = null
    private var pluginLibraryLoaded = false
    private var appsLoaded = false
    private var appsLoadJob: Job? = null
    private val logLock = Any()
    private var logProcess: Process? = null
    private var logSerial = 0
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
    private var packageEventSocket: PluginHookEventSocket? = null
    private val packageSocketLeaseLocks = ConcurrentHashMap<String, Mutex>()
    @Volatile
    private var packageEventSerial = 0

    init {
        refresh()
        loadInstalledApps()
    }

    fun refresh() {
        if (refreshJob?.isActive == true || _state.value.scopeSavingId != null) return
        refreshJob = viewModelScope.launch {
            _state.update { it.copy(loading = true) }
            val root = withContext(Dispatchers.IO) { RootShell.isRoot() }
            if (!root) {
                updateSelectedTarget(null)
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
            val config = if (prop != null) withContext(Dispatchers.IO) {
                runCatching { scopeStore.read() }
            } else null
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
            config?.fold(
                onSuccess = { snapshot ->
                    _state.update { it.copy(targets = snapshot.targets, pluginScopes = snapshot.scopes) }
                    val id = _state.value.activePluginId
                    // Keep an already inspected process reachable for explicit stop/unload after scope removal.
                    val selected = _state.value.selectedTarget ?: id?.let { snapshot.scopes[it]?.firstOrNull() }
                    updateSelectedTarget(selected)
                },
                onFailure = { message(text(R.string.msg_unable_write_targets) + ": " + it.message.orEmpty(), error = true) },
            )
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

    private fun operationBusy(): Boolean = _state.value.pluginOperationInProgress

    fun selectPlugin(id: String): Boolean {
        if (operationBusy()) return false
        closePackageEvents()
        val current = _state.value
        if (current.activePluginId == id) return true
        val target = pluginTargets[id]
            ?: current.pluginScopes[id]?.firstOrNull()
        _state.update { it.copy(activePluginId = id) }
        if (target != null) pluginTargets[id] = target
        updateSelectedTarget(target)
        return true
    }

    fun leavePlugin() {
        if (operationBusy()) return
        closePackageEvents()
        _state.update { it.copy(activePluginId = null) }
        updateSelectedTarget(null)
    }

    fun selectTarget(process: String) {
        if (operationBusy()) return
        val current = _state.value
        val id = current.activePluginId ?: return
        if (process !in current.pluginScopes[id].orEmpty()) return
        pluginTargets[id] = process
        updateSelectedTarget(process)
    }

    private fun updateSelectedTarget(target: String?) {
        if (_state.value.selectedTarget != target) {
            closePackageEvents()
            ++packageActionSerial
            ++nativeActionSerial
            ++pluginRequestSerial
            pluginCancellation?.cancel()
            pluginCancellation = null
            pluginJob?.cancel()
        }
        _state.update { it.withSelectedTarget(target) }
    }

    fun savePluginScope(id: String, targets: List<String>) {
        if (operationBusy() || _state.value.activePluginId != id) return
        _state.update { it.copy(scopeSavingId = id) }
        closePackageEvents()
        viewModelScope.launch {
            try {
                val snapshot = withContext(Dispatchers.IO) { scopeStore.setScope(id, targets) }
                _state.update { it.copy(targets = snapshot.targets, pluginScopes = snapshot.scopes) }
                if (_state.value.selectedTarget == null) {
                    val target = snapshot.scopes[id]?.firstOrNull()
                    if (target != null) pluginTargets[id] = target
                    updateSelectedTarget(target)
                }
                message(text(R.string.plugin_scope_saved))
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                message(text(R.string.msg_unable_write_targets) + ": " + error.message.orEmpty(), error = true)
            } finally {
                _state.update { it.copy(scopeSavingId = null) }
                refreshPluginRuntime()
            }
        }
    }

    fun refreshPluginRuntime() {
        val current = _state.value
        if (current.pluginOperationInProgress) return
        val target = current.selectedTarget ?: return
        val id = current.activePluginId ?: return
        val native = current.nativeLibrary.any { it.artifact.id == id }
        if (!native && current.directoryPlugins.none { it.plugin.manifest.id == id }) return
        val key = target to native
        if (pluginJob?.isActive == true && pluginRuntimeKey == key) return
        pluginRuntimeKey = key
        val serial = ++pluginRequestSerial
        pluginCancellation?.cancel()
        pluginJob?.cancel()
        val cancellation = RpcCancellation()
        pluginCancellation = cancellation
        pluginJob = viewModelScope.launch {
            _state.update { it.copy(pluginsLoading = true) }
            try {
                val raw = withContext(Dispatchers.IO) {
                    DejavuRpc.execute(target, if (native) "return plugin.list()" else LuaPackageScripts.snapshot(), cancellation)
                }
                if (serial != pluginRequestSerial || _state.value.selectedTarget != target) return@launch
                if (native) {
                    _state.update {
                        bindNativeRuntime(it.copy(
                            pluginsLoading = false, pluginsRaw = raw, plugins = parsePlugins(raw),
                            nativeRuntimeKnown = true,
                        ))
                    }
                } else {
                    val snapshot = parsePackageRuntimeSnapshot(raw)
                    _state.update {
                        it.copy(pluginsLoading = false, directoryRuntimeSnapshot = snapshot,
                            directoryRuntimeError = null, directoryTargetOffline = false)
                            .withDirectoryPackages(it.directoryPlugins.map { row -> row.plugin })
                    }
                    closeStalePackageEvents(target)
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                if (serial != pluginRequestSerial || _state.value.selectedTarget != target) return@launch
                if (native) {
                    _state.update {
                        bindNativeRuntime(it.copy(pluginsLoading = false, pluginsRaw = "", plugins = emptyList(),
                            nativeRuntimeKnown = false))
                    }
                    if (error !is TargetProcessNotRunning) {
                        message(text(R.string.msg_plugin_list_failed, error.message.orEmpty()), error = true)
                    }
                } else {
                    _state.update {
                        it.copy(pluginsLoading = false, directoryRuntimeSnapshot = null,
                            directoryRuntimeError = runtimeError(error),
                            directoryTargetOffline = error is TargetProcessNotRunning)
                            .withDirectoryPackages(it.directoryPlugins.map { row -> row.plugin })
                    }
                    closeStalePackageEvents(target)
                }
            }
        }
    }

    private fun closeStalePackageEvents(target: String?) {
        val current = _state.value
        current.packageEvents?.let { events ->
            if (current.directoryPlugins.none { row ->
                    row.runState == DirectoryPluginState.RUNNING &&
                        events.matches(target, row.plugin.manifest.id, row.session)
                }) closePackageEvents()
        }
    }

    fun unloadPlugin(target: String, name: String) {
        val current = _state.value
        if (current.selectedTarget != target || operationBusy() || current.pluginsLoading) return
        val row = current.nativeLibrary.firstOrNull {
            it.artifact.id == current.activePluginId && it.runtime?.name == name
        } ?: return
        val request = ++nativeActionSerial
        _state.update { it.copy(nativePluginActionPath = row.artifact.path) }
        viewModelScope.launch {
            try {
                val result = withContext(Dispatchers.IO) { DejavuRpc.unloadPlugin(target, name) }
                if (request == nativeActionSerial && _state.value.selectedTarget == target) message(result)
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                if (request == nativeActionSerial && _state.value.selectedTarget == target) {
                    message(text(R.string.msg_unload_failed, error.message.orEmpty()), error = true)
                }
            } finally {
                if (request == nativeActionSerial && _state.value.selectedTarget == target) {
                    _state.update { it.copy(nativePluginActionPath = null) }
                    refreshPluginRuntime()
                }
            }
        }
    }

    suspend fun pluginSkill(target: String, name: String): Result<String> {
        val current = _state.value
        if (current.selectedTarget != target || current.nativeLibrary.none {
                it.artifact.id == current.activePluginId && it.runtime?.name == name
            }) return Result.failure(RpcException("Selected plugin changed"))
        return withContext(Dispatchers.IO) { runCatching { DejavuRpc.pluginSkill(target, name) } }
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
        if (current.selectedTarget != target || current.pluginOperationInProgress ||
            current.pluginsLoading || current.pluginDirectoryLoading) return
        val artifact = current.nativeLibrary.firstOrNull { it.artifact.path == path }?.artifact ?: return
        if (artifact.id?.let { current.canOperatePlugin(it, target) } != true) return
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
                    refreshPluginRuntime()
                }
            }
        }
    }

    fun refreshPluginLibrary(force: Boolean = true) {
        if (_state.value.pluginOperationInProgress || directoryJob?.isActive == true ||
            (!force && pluginLibraryLoaded)) return
        val serial = ++directoryRequestSerial
        directoryJob = viewModelScope.launch {
            _state.update { it.copy(pluginDirectoryLoading = true, pluginDirectoryError = null) }
            try {
                val (packages, artifacts, scanErrors) = withContext(Dispatchers.IO) {
                    val scan = PluginLibraryScan.read()
                    val packages = packageImporter.list(scan)
                    val artifacts = nativeLibrary.list(scan)
                    Triple(packages, artifacts, (packageImporter.lastScanErrors + nativeLibrary.lastScanErrors).distinct())
                }
                if (serial != directoryRequestSerial) return@launch
                pluginLibraryLoaded = scanErrors.isEmpty()
                _state.update { previous ->
                    bindNativeRuntime(previous.copy(
                        nativeLibrary = artifacts.map { artifact ->
                            NativeLibraryPluginUi(artifact, error = previous.nativeLibrary
                                .firstOrNull { it.artifact == artifact }?.error)
                        },
                        pluginDirectoryLoading = false,
                        pluginDirectoryError = scanErrors.takeIf { it.isNotEmpty() }?.joinToString("\n"),
                    ).withDirectoryPackages(packages))
                }
                closeStalePackageEvents(_state.value.selectedTarget)
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

    fun importPluginZip(uri: Uri) {
        if (operationBusy()) return
        viewModelScope.launch {
            _state.update { it.copy(pluginImporting = true) }
            try {
                val importedName = withContext(Dispatchers.IO) {
                    val archive = getApplication<Application>().contentResolver.openInputStream(uri)?.use {
                        PluginZipArchive.read(it)
                    } ?: error("Unable to open the plugin ZIP")
                    if (archive.manifest.kind == "lua") {
                        val pkg = ImportedPluginPackage(archive.manifest, archive.files.mapValues { decodePluginUtf8(it.value) })
                        packageImporter.importPackage(pkg, ::canReplaceDirectoryPlugin).manifest.name
                    } else nativeLibrary.importPackage(archive.manifest, archive.files, ::canReplaceDirectoryPlugin)
                        .joinToString { it.name }
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

    private fun canReplaceDirectoryPlugin(id: String): Boolean {
        if (_state.value.directoryPlugins.none { it.plugin.manifest.id == id }) return true
        // A plugin may still run in a process removed from its scope. Include live RPC metadata,
        // so replacing sources never relies only on the currently displayed process.
        val listing = RootShell.run("ls -1 ${RootShell.quote("$MODULE_DIR/run")}")
        check(listing.exitCode == 0) { "Cannot inspect running plugin processes: ${listing.output.trim().take(160)}" }
        val endpointName = Regex("^([A-Za-z0-9_.:-]{1,128})\\.[1-9][0-9]*\\.rpc$")
        val processes = (_state.value.pluginScopes[id].orEmpty() + listing.output.lineSequence().mapNotNull {
            endpointName.matchEntire(it.trim())?.groupValues?.get(1)
        }.toList()).distinct()
        return processes.all { process ->
            try {
                val snapshot = parsePackageRuntimeSnapshot(DejavuRpc.execute(process, LuaPackageScripts.snapshot()))
                snapshot[id]?.state.let { it == null || it == DirectoryPluginState.STOPPED }
            } catch (_: TargetProcessNotRunning) {
                true
            } catch (error: Exception) {
                throw RpcException("Cannot verify plugin $id in $process: ${error.message.orEmpty()}")
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
        if (current.activePluginId != id || current.scopeSavingId != null ||
            current.pluginsLoading || current.pluginDirectoryLoading) return
        if (start && (!current.canOperatePlugin(id, target) || row.runState != DirectoryPluginState.STOPPED)) return
        val session = if (start) newPackageToken() else row.session
        if (!start && current.packageEvents?.packageId == id) closePackageEvents()
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
                    refreshPluginRuntime()
                }
            }
        }
    }

    fun runDirectoryPluginAction(target: String, id: String, action: String) {
        val current = _state.value
        if (current.selectedTarget != target || current.pluginActionId != null || current.pluginImporting ||
            current.nativePluginActionPath != null) return
        val row = current.directoryPlugins.firstOrNull { it.plugin.manifest.id == id } ?: return
        if (!current.canOperatePlugin(id, target)) return
        val session = row.session ?: return
        if (row.runState != DirectoryPluginState.RUNNING) return
        val page = row.plugin.page ?: return
        if (!page.hasAction(action)) return
        if (row.plugin.manifest.events?.usesSocket == true &&
            page.eventLimit != null &&
            current.packageEvents?.let { it.matches(target, id, session) && it.connected } != true) return
        val request = ++packageActionSerial
        _state.update { it.copy(pluginActionId = id) }
        viewModelScope.launch {
            try {
                withContext(Dispatchers.IO) {
                    DejavuRpc.execute(target, LuaPackageScripts.runAction(id, session, action))
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (_: Exception) {
                if (request == packageActionSerial) message(text(R.string.package_action_failed), error = true)
            } finally {
                if (request == packageActionSerial) _state.update { it.copy(pluginActionId = null) }
            }
        }
    }

    fun setPluginListenPort(target: String, id: String, port: Int) {
        if (!validPluginListenPort(port)) return
        _state.update { state ->
            if (state.selectedTarget != target || state.directoryPlugins.none {
                    it.plugin.manifest.id == id && it.plugin.manifest.events?.usesSocket == true &&
                        it.plugin.page?.preset is PluginPagePreset.Monitor
                }) return@update state
            val key = id to target
            state.copy(pluginListenPorts = if (port == 0) state.pluginListenPorts - key
                else state.pluginListenPorts + (key to port))
        }
    }

    fun openPackageEvents(target: String, id: String, listenPort: Int? = null) {
        if (!_state.value.canOperatePlugin(id, target)) return
        val row = _state.value.directoryPlugins.firstOrNull { it.plugin.manifest.id == id } ?: return
        val spec = row.plugin.manifest.events ?: return
        val session = row.session ?: return
        if (row.runState != DirectoryPluginState.RUNNING) return
        closePackageEvents()
        val serial = packageEventSerial
        val openedAt = Instant.now()
        val since = "${openedAt.epochSecond}.${openedAt.nano.toString().padStart(9, '0')}"
        val decoder = if (spec.usesSocket) null else PluginEventDecoder(spec.marker, session)
        val limit = row.plugin.page?.eventLimit ?: 50
        _state.update { it.copy(packageEvents = PackageEventsUi(id, row.plugin.manifest.name, target, session = session)) }
        packageEventJob = viewModelScope.launch(Dispatchers.IO) {
            var process: Process? = null
            var expiryJob: Job? = null
            val seen = linkedMapOf<String, Long>()
            fun deliver(payload: String) {
                if (serial != packageEventSerial) return
                val event = try { JSONObject(payload) } catch (_: Exception) { return }
                val projected = projectPluginEvent(event, spec, row.plugin.manifest.name)
                val now = android.os.SystemClock.elapsedRealtime()
                seen.entries.removeAll { now - it.value > 30_000 }
                val key = projected.dedupKey
                if (key != null && seen.put(key, now) != null) return
                if (seen.size > 4096) seen.remove(seen.keys.first())
                val item = projected.item.copy(receivedAtMillis = System.currentTimeMillis())
                updatePackageEvents(serial) {
                    it.copy(events = (it.events + item).takeLast(limit))
                }
            }
            try {
                if (decoder == null) {
                    packageSocketLeaseLocks.getOrPut(target) { Mutex() }.withLock {
                        if (serial == packageEventSerial) {
                            readPackageSocket(target, id, session, requireNotNull(spec.topic), listenPort ?: 0, serial, ::deliver)
                        }
                    }
                } else {
                    val pid = DejavuRpc.processPid(target)
                    if (serial != packageEventSerial) return@launch
                    process = RootShell.startLogcat(
                        "--pid $pid -v raw -T $since -s ${RootShell.quote(spec.tag + ":I")} '*:S'",
                    )
                    synchronized(packageEventLock) {
                        if (serial != packageEventSerial) return@launch
                        packageEventProcess = process
                    }
                    updatePackageEvents(serial) { it.copy(connected = true) }
                    expiryJob = launch(Dispatchers.Default) {
                        while (isActive) {
                            delay(1000)
                            decoder.feed("")
                        }
                    }
                    process.inputStream.bufferedReader().useLines { lines ->
                        lines.forEach { line ->
                            if (!isActive || serial != packageEventSerial) return@launch
                            decoder.feed(line)?.let(::deliver)
                        }
                    }
                }
                if (serial == packageEventSerial) error("Live event stream closed")
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                updatePackageEvents(serial) {
                    it.copy(connected = false, error = text(R.string.package_events_failed, error.message.orEmpty()))
                }
            } finally {
                expiryJob?.cancel()
                RootShell.stopLogcat(process)
                decoder?.clear()
                seen.clear()
                synchronized(packageEventLock) {
                    if (packageEventProcess === process) packageEventProcess = null
                }
            }
        }
    }

    private suspend fun readPackageSocket(
        target: String,
        id: String,
        session: String,
        topic: String,
        port: Int,
        serial: Int,
        onPayload: (String) -> Unit,
    ) {
        val owner = newPackageToken()
        val socket = PluginHookEventSocket(port)
        var attaching = false
        try {
            synchronized(packageEventLock) {
                if (serial != packageEventSerial) return
                packageEventSocket = socket
            }
            updatePackageEvents(serial) { it.copy(listenPort = socket.listenPort) }
            currentCoroutineContext().ensureActive()
            attaching = true
            DejavuRpc.execute(target, LuaPackageScripts.attachSocket(id, session, owner, socket.listenPort))
            currentCoroutineContext().ensureActive()
            socket.readEvents("$topic.$session", {
                updatePackageEvents(serial) { it.copy(connected = true) }
            }, onPayload)
        } finally {
            socket.stopReading()
            try {
                if (attaching) {
                    withContext(NonCancellable) {
                        runCatching { DejavuRpc.execute(target, LuaPackageScripts.detachSocket(id, session, owner)) }
                    }
                }
            } finally {
                runCatching { socket.close() }
                synchronized(packageEventLock) {
                    if (packageEventSocket === socket) packageEventSocket = null
                }
            }
        }
    }

    private fun updatePackageEvents(serial: Int, update: (PackageEventsUi) -> PackageEventsUi) {
        _state.update { state ->
            if (serial == packageEventSerial) state.copy(packageEvents = state.packageEvents?.let(update))
            else state
        }
    }

    private fun newPackageToken(): String = ByteArray(8).also { SecureRandom().nextBytes(it) }
        .joinToString("") { "%02x".format(it.toInt() and 0xff) }

    fun closePackageEvents() {
        val process = synchronized(packageEventLock) {
            ++packageEventSerial
            packageEventSocket?.stopReading()
            packageEventProcess.also { packageEventProcess = null }
        }
        RootShell.stopLogcat(process)
        packageEventJob?.cancel()
        packageEventJob = null
        _state.update { it.copy(packageEvents = null) }
    }

    fun startLogs() {
        if (logJob?.isActive == true) return
        val serial = synchronized(logLock) {
            logBuilder.setLength(0)
            _state.update { it.copy(logText = "", logcatRunning = true) }
            ++logSerial
        }
        logJob = viewModelScope.launch(Dispatchers.IO) {
            var process: Process? = null
            try {
                process = RootShell.startLogcat("-v time -s $LOG_TAGS")
                synchronized(logLock) {
                    if (serial != logSerial || !isActive) return@launch
                    logProcess = process
                }
                var lastEmit = 0L
                process.inputStream.bufferedReader().useLines { lines ->
                    lines.forEach { line ->
                        // Package events have their own clear-on-close viewer;
                        // keep message frames out of the general log buffer.
                        if (_state.value.directoryPlugins.any { item ->
                                item.plugin.manifest.events?.marker?.let { line.contains("$it ") } == true
                            }) return@forEach
                        synchronized(logLock) {
                            if (serial != logSerial || !isActive) return@launch
                            logBuilder.appendLine(line)
                            if (logBuilder.length > 80_000) {
                                logBuilder.delete(0, logBuilder.length - 80_000)
                            }
                            val now = android.os.SystemClock.elapsedRealtime()
                            if (now - lastEmit >= 120) {
                                lastEmit = now
                                val snapshot = logBuilder.toString()
                                _state.update { it.copy(logText = snapshot) }
                            }
                        }
                    }
                }
            } catch (error: Exception) {
                synchronized(logLock) {
                    if (serial == logSerial && process == null) {
                        message(text(R.string.msg_logcat_failed, error.message.orEmpty()), error = true)
                    }
                }
            } finally {
                RootShell.stopLogcat(process)
                synchronized(logLock) {
                    if (serial == logSerial) {
                        logProcess = null
                        _state.update { it.copy(logcatRunning = false, logText = logBuilder.toString()) }
                    }
                }
            }
        }
    }

    fun stopLogs() {
        if (logJob == null) return
        val process = synchronized(logLock) {
            ++logSerial
            _state.update { it.copy(logcatRunning = false, logText = logBuilder.toString()) }
            logProcess.also { logProcess = null }
        }
        RootShell.stopLogcat(process)
        logJob?.cancel()
        logJob = null
    }

    fun clearLogs() {
        synchronized(logLock) {
            logBuilder.setLength(0)
            _state.update { it.copy(logText = "") }
        }
    }

    fun message(text: String, error: Boolean = false) {
        _state.update { it.copy(lastMessage = text, lastError = error) }
    }

    private fun text(@StringRes id: Int, vararg args: Any): String =
        ManagerLanguage.localizedContext(getApplication<Application>())
            .getString(id, *args)

    private fun runtimeError(error: Throwable): String =
        if (error is TargetProcessNotRunning) text(R.string.target_process_offline, error.process)
        else error.message.orEmpty()

    fun consumeMessage() {
        _state.update { it.copy(lastMessage = null) }
    }

    override fun onCleared() {
        pluginCancellation?.cancel()
        closePackageEvents()
        stopLogs()
        super.onCleared()
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
