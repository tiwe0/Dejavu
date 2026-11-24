package cafe.ivory.dejavu.data

import android.app.Application
import android.net.Uri
import androidx.annotation.StringRes
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import cafe.ivory.dejavu.ManagerLanguage
import cafe.ivory.dejavu.R
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

internal class PluginsViewModel(
    private val application: Application,
    private val shell: ShellExecutor = RootShell,
    private val rpc: DejavuRpcClient = DejavuRpcClient(shell),
    private val packageImporter: PluginPackageImporter = PluginPackageImporter(application, shell),
    private val nativeLibrary: NativePluginLibrary = NativePluginLibrary(application, shell),
    private val scopesProvider: () -> Map<String, List<String>> = { emptyMap() },
    private val scopeSavingIdProvider: () -> String? = { null },
    private val onMessage: (String, Boolean) -> Unit = { _, _ -> },
    private val coroutineScope: CoroutineScope? = null,
) : ViewModel() {
    private val scope get() = coroutineScope ?: viewModelScope
    private val _state = MutableStateFlow(PluginsUiState())
    val state: StateFlow<PluginsUiState> = _state.asStateFlow()

    private val pluginTargets = mutableMapOf<String, String>()
    private var pluginJob: Job? = null
    private var pluginCancellation: RpcCancellation? = null
    private var pluginRequestSerial = 0
    private var pluginRuntimeKey: Pair<String, Boolean>? = null
    private var pluginLibraryLoaded = false
    private var directoryJob: Job? = null
    private var directoryRequestSerial = 0
    private var nativeActionSerial = 0
    private var packageActionSerial = 0
    private var skillJob: Job? = null

    private val eventStream = PluginEventStreamController(
        scope = scope,
        rpc = rpc,
        shell = shell,
        formatError = { detail -> text(R.string.package_events_failed, detail) },
        onStateUpdate = { update -> _state.update { it.copy(packageEvents = update(it.packageEvents)) } },
    )

    fun excludedLogMarkers(): Set<String> =
        _state.value.directoryPlugins.mapNotNull { it.plugin.manifest.events?.marker }.toSet()

    private fun operationBusy(): Boolean = _state.value.operationInProgress(scopeSavingIdProvider())

    /**
     * Atomically selects a plugin and optional target process so callers (e.g. Overview rows)
     * can navigate directly without an intermediate frame.
     */
    fun selectPlugin(id: String, preferredTarget: String? = null): Boolean {
        if (operationBusy()) return false
        closePackageEvents()
        val scopes = scopesProvider()[id].orEmpty()
        val target = preferredTarget?.takeIf { it in scopes }
            ?: pluginTargets[id]?.takeIf { it in scopes }
            ?: scopes.firstOrNull()
        _state.update { it.copy(activePluginId = id) }
        if (target != null) pluginTargets[id] = target
        updateSelectedTarget(target)
        return true
    }

    fun leavePlugin() {
        if (operationBusy()) return
        closePackageEvents()
        dismissSkill()
        _state.update { it.copy(activePluginId = null) }
        updateSelectedTarget(null)
    }

    fun selectTarget(process: String) {
        if (operationBusy()) return
        val id = _state.value.activePluginId ?: return
        if (process !in scopesProvider()[id].orEmpty()) return
        pluginTargets[id] = process
        updateSelectedTarget(process)
    }

    fun onScopesUpdated(scopes: Map<String, List<String>>) {
        val id = _state.value.activePluginId ?: return
        // Keep an already inspected process reachable for explicit stop/unload after scope removal.
        if (_state.value.selectedTarget == null) {
            val target = scopes[id]?.firstOrNull()
            if (target != null) {
                pluginTargets[id] = target
                updateSelectedTarget(target)
            }
        }
    }

    fun onRootLost() {
        closePackageEvents()
        dismissSkill()
        updateSelectedTarget(null)
    }

    private fun updateSelectedTarget(target: String?) {
        if (_state.value.selectedTarget != target) {
            closePackageEvents()
            dismissSkill()
            ++packageActionSerial
            ++nativeActionSerial
            ++pluginRequestSerial
            pluginCancellation?.cancel()
            pluginCancellation = null
            pluginJob?.cancel()
        }
        _state.update { it.withSelectedTarget(target) }
    }

    fun refreshPluginRuntime() {
        val current = _state.value
        if (operationBusy()) return
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
        pluginJob = scope.launch {
            _state.update { it.copy(pluginsLoading = true) }
            try {
                val raw = withContext(Dispatchers.IO) {
                    rpc.execute(target, if (native) "return plugin.list()" else LuaPackageScripts.snapshot(), cancellation)
                }
                if (serial != pluginRequestSerial || _state.value.selectedTarget != target) return@launch
                if (native) {
                    _state.update {
                        bindNativeRuntime(it.copy(
                            pluginsLoading = false,
                            plugins = parsePlugins(raw),
                            nativeRuntimeKnown = true,
                            directoryRuntimeError = null,
                            directoryTargetOffline = false,
                        ))
                    }
                } else {
                    val snapshot = parsePackageRuntimeSnapshot(raw)
                    _state.update {
                        it.copy(
                            pluginsLoading = false,
                            directoryRuntimeSnapshot = snapshot,
                            directoryRuntimeError = null,
                            directoryTargetOffline = false,
                        ).withDirectoryPackages(it.directoryPlugins.map { row -> row.plugin })
                    }
                    closeStalePackageEvents(target)
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                if (serial != pluginRequestSerial || _state.value.selectedTarget != target) return@launch
                if (native) {
                    _state.update {
                        bindNativeRuntime(it.copy(
                            pluginsLoading = false,
                            plugins = emptyList(),
                            nativeRuntimeKnown = false,
                            directoryRuntimeError = runtimeError(error),
                            directoryTargetOffline = error is TargetProcessNotRunning,
                        ))
                    }
                    if (error !is TargetProcessNotRunning) {
                        onMessage(text(R.string.msg_plugin_list_failed, error.message.orEmpty()), true)
                    }
                } else {
                    _state.update {
                        it.copy(
                            pluginsLoading = false,
                            directoryRuntimeSnapshot = null,
                            directoryRuntimeError = runtimeError(error),
                            directoryTargetOffline = error is TargetProcessNotRunning,
                        ).withDirectoryPackages(it.directoryPlugins.map { row -> row.plugin })
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
        scope.launch {
            try {
                val result = withContext(Dispatchers.IO) { rpc.unloadPlugin(target, name) }
                if (request == nativeActionSerial && _state.value.selectedTarget == target) onMessage(result, false)
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                if (request == nativeActionSerial && _state.value.selectedTarget == target) {
                    onMessage(text(R.string.msg_unload_failed, error.message.orEmpty()), true)
                }
            } finally {
                if (request == nativeActionSerial && _state.value.selectedTarget == target) {
                    _state.update { it.copy(nativePluginActionPath = null) }
                    refreshPluginRuntime()
                }
            }
        }
    }

    fun loadSkill(target: String, name: String) {
        val current = _state.value
        if (current.selectedTarget != target || current.nativeLibrary.none {
                it.artifact.id == current.activePluginId && it.runtime?.name == name
            }) return
        skillJob?.cancel()
        _state.update { it.copy(skillSheet = SkillSheetState(loading = true, text = "")) }
        skillJob = scope.launch {
            val result = withContext(Dispatchers.IO) { runCatching { rpc.pluginSkill(target, name) } }
            val fallback = text(R.string.msg_skill_load_failed)
            _state.update { state ->
                if (state.skillSheet == null) state
                else state.copy(skillSheet = SkillSheetState(loading = false, text = result.fold({ it }, { it.message ?: fallback })))
            }
        }
    }

    fun dismissSkill() {
        skillJob?.cancel()
        skillJob = null
        _state.update { if (it.skillSheet == null) it else it.copy(skillSheet = null) }
    }

    private fun bindNativeRuntime(state: PluginsUiState): PluginsUiState = state.copy(
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
        if (current.selectedTarget != target || operationBusy() ||
            current.pluginsLoading || current.pluginDirectoryLoading) return
        val artifact = current.nativeLibrary.firstOrNull { it.artifact.path == path }?.artifact ?: return
        if (artifact.id?.let { current.canOperatePlugin(it, target, scopesProvider(), scopeSavingIdProvider()) } != true) return
        val request = ++nativeActionSerial
        _state.update { state ->
            state.copy(
                nativePluginActionPath = path,
                nativeLibrary = state.nativeLibrary.map {
                    if (it.artifact.path == path) it.copy(error = null) else it
                },
            )
        }
        scope.launch {
            try {
                val result = withContext(Dispatchers.IO) {
                    val deployed = nativeLibrary.deploy(target, artifact)
                    rpc.loadPlugin(target, deployed)
                }
                if (request == nativeActionSerial && _state.value.selectedTarget == target) onMessage(result.take(300), false)
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                if (request == nativeActionSerial && _state.value.selectedTarget == target) {
                    val detail = text(R.string.msg_load_failed, error.message.orEmpty())
                    _state.update { state ->
                        state.copy(nativeLibrary = state.nativeLibrary.map {
                            if (it.artifact.path == path) it.copy(error = detail) else it
                        })
                    }
                    onMessage(detail, true)
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
        if (operationBusy() || directoryJob?.isActive == true || (!force && pluginLibraryLoaded)) return
        val serial = ++directoryRequestSerial
        directoryJob = scope.launch {
            _state.update { it.copy(pluginDirectoryLoading = true, pluginDirectoryError = null) }
            try {
                val (packages, artifacts, scanErrors) = withContext(Dispatchers.IO) {
                    val scan = PluginLibraryScan.read(shell)
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
                    it.copy(
                        pluginDirectoryLoading = false,
                        pluginDirectoryError = text(R.string.package_scan_failed, error.message.orEmpty()),
                    )
                }
            }
        }
    }

    fun importPluginZip(uri: Uri) {
        if (operationBusy()) return
        // Cancel any in-flight scan so the post-import refresh is never skipped.
        ++directoryRequestSerial
        directoryJob?.cancel()
        directoryJob = null
        scope.launch {
            _state.update { it.copy(pluginImporting = true, pluginDirectoryLoading = false) }
            try {
                val importedName = withContext(Dispatchers.IO) {
                    val archive = application.contentResolver.openInputStream(uri)?.use {
                        PluginZipArchive.read(it)
                    } ?: error("Unable to open the plugin ZIP")
                    if (archive.manifest.kind == "lua") {
                        val pkg = ImportedPluginPackage(archive.manifest, archive.files.mapValues { decodePluginUtf8(it.value) })
                        packageImporter.importPackage(pkg, ::canReplaceDirectoryPlugin).manifest.name
                    } else {
                        nativeLibrary.importPackage(archive.manifest, archive.files, ::canReplaceDirectoryPlugin)
                            .joinToString { it.name }
                    }
                }
                onMessage(text(R.string.package_import_success, importedName), false)
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                onMessage(text(R.string.package_import_failed, error.message.orEmpty()), true)
            } finally {
                _state.update { it.copy(pluginImporting = false) }
                refreshPluginLibrary(force = true)
            }
        }
    }

    private fun canReplaceDirectoryPlugin(id: String): Boolean {
        // Check disk directly rather than relying on the in-memory UI list (which may be empty on scan failure).
        val directory = "$DEFAULT_PLUGIN_DIRECTORY/$id"
        val exists = shell.run("if [ -e ${shell.quote(directory)} ] || [ -L ${shell.quote(directory)} ]; then printf existing; fi")
        check(exists.exitCode == 0) { "Cannot inspect existing plugin directory" }
        if (exists.output.isEmpty()) return true
        // A plugin may still run in a process removed from its scope. Include live RPC metadata,
        // so replacing sources never relies only on the currently displayed process.
        val processes = (scopesProvider()[id].orEmpty() + rpc.listActiveRpcProcesses()).distinct()
        return processes.all { process ->
            try {
                val snapshot = parsePackageRuntimeSnapshot(rpc.execute(process, LuaPackageScripts.snapshot()))
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
        if (current.selectedTarget != target || operationBusy()) return
        val row = current.directoryPlugins.firstOrNull { it.plugin.manifest.id == id } ?: return
        if (current.activePluginId != id || current.pluginsLoading || current.pluginDirectoryLoading) return
        if (start && (!current.canOperatePlugin(id, target, scopesProvider(), scopeSavingIdProvider()) ||
                row.runState != DirectoryPluginState.STOPPED)) return
        val session = if (start) PluginEventStreamController.newToken() else row.session
        if (!start && current.packageEvents?.packageId == id) closePackageEvents()
        val request = ++packageActionSerial
        ++directoryRequestSerial
        directoryJob?.cancel()
        _state.update { state ->
            state.copy(
                pluginActionId = id,
                pluginDirectoryLoading = false,
                directoryPlugins = state.directoryPlugins.map {
                    if (it.plugin.manifest.id == id) it.copy(error = null) else it
                },
            )
        }
        scope.launch {
            try {
                val script = if (start) LuaPackageScripts.start(row.plugin, session!!)
                else LuaPackageScripts.stop(id, session)
                val result = withContext(Dispatchers.IO) { rpc.execute(target, script) }
                if (request == packageActionSerial && _state.value.selectedTarget == target) {
                    onMessage(result.take(300), false)
                    if (!start && _state.value.packageEvents?.packageId == id) closePackageEvents()
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                if (request == packageActionSerial && _state.value.selectedTarget == target) {
                    val detail = text(
                        if (start) R.string.package_start_failed else R.string.package_stop_failed,
                        error.message.orEmpty(),
                    )
                    _state.update { state ->
                        state.copy(directoryPlugins = state.directoryPlugins.map {
                            if (it.plugin.manifest.id == id) it.copy(
                                runState = DirectoryPluginState.ERROR, session = session, error = detail,
                            ) else it
                        })
                    }
                    onMessage(detail, true)
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
        if (current.selectedTarget != target || operationBusy()) return
        val row = current.directoryPlugins.firstOrNull { it.plugin.manifest.id == id } ?: return
        if (!current.canOperatePlugin(id, target, scopesProvider(), scopeSavingIdProvider())) return
        val session = row.session ?: return
        if (row.runState != DirectoryPluginState.RUNNING) return
        val page = row.plugin.page ?: return
        if (!page.hasAction(action)) return
        if (row.plugin.manifest.events?.usesSocket == true &&
            page.eventLimit != null &&
            current.packageEvents?.let { it.matches(target, id, session) && it.connected } != true) return
        val request = ++packageActionSerial
        _state.update { it.copy(pluginActionId = id) }
        scope.launch {
            try {
                withContext(Dispatchers.IO) {
                    rpc.execute(target, LuaPackageScripts.runAction(id, session, action))
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (_: Exception) {
                if (request == packageActionSerial) onMessage(text(R.string.package_action_failed), true)
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
                        it.plugin.page?.eventLimit != null
                }) return@update state
            val key = id to target
            state.copy(pluginListenPorts = if (port == 0) state.pluginListenPorts - key
            else state.pluginListenPorts + (key to port))
        }
    }

    fun openPackageEvents(target: String, id: String, listenPort: Int? = null) {
        if (!_state.value.canOperatePlugin(id, target, scopesProvider(), scopeSavingIdProvider())) return
        val row = _state.value.directoryPlugins.firstOrNull { it.plugin.manifest.id == id } ?: return
        eventStream.open(row, target, listenPort)
    }

    fun closePackageEvents() {
        eventStream.close()
    }

    private fun text(@StringRes id: Int, vararg args: Any): String =
        ManagerLanguage.localizedContext(application).getString(id, *args)

    private fun runtimeError(error: Throwable): String =
        if (error is TargetProcessNotRunning) text(R.string.target_process_offline, error.process)
        else error.message.orEmpty()

    override fun onCleared() {
        pluginCancellation?.cancel()
        dismissSkill()
        closePackageEvents()
        super.onCleared()
    }

    companion object {
        fun factory(
            application: Application,
            scopesProvider: () -> Map<String, List<String>>,
            scopeSavingIdProvider: () -> String?,
            onMessage: (String, Boolean) -> Unit,
        ): ViewModelProvider.Factory = object : ViewModelProvider.Factory {
            @Suppress("UNCHECKED_CAST")
            override fun <T : ViewModel> create(modelClass: Class<T>): T =
                PluginsViewModel(
                    application = application,
                    scopesProvider = scopesProvider,
                    scopeSavingIdProvider = scopeSavingIdProvider,
                    onMessage = onMessage,
                ) as T
        }

        internal fun parsePlugins(raw: String): List<PluginInfo> {
            val regex = Regex(
                """\[([A-Za-z0-9_.-]+)(?: ([^ ]+))? loaded=(yes|no) jit=(yes|no) caps=(0x[0-9a-fA-F]+) skill=(yes|no) path=(.*?)\]""",
            )
            return regex.findAll(raw).map { match ->
                PluginInfo(
                    name = match.groupValues[1],
                    version = match.groupValues[2],
                    loaded = match.groupValues[3] == "yes",
                    jit = match.groupValues[4] == "yes",
                    skill = match.groupValues[6] == "yes",
                    path = match.groupValues[7],
                )
            }.toList()
        }
    }
}
