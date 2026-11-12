package cafe.ivory.dejavu.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.Article
import androidx.compose.material.icons.rounded.DeleteOutline
import androidx.compose.material.icons.rounded.Add
import androidx.compose.material.icons.rounded.Lock
import androidx.compose.material.icons.rounded.PlayArrow
import androidx.compose.material.icons.rounded.Search
import androidx.compose.material.icons.rounded.Stop
import androidx.compose.material.icons.rounded.VerticalAlignBottom
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.Tab
import androidx.compose.material3.TabRow
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.setValue
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.disabled
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import cafe.ivory.dejavu.R
import cafe.ivory.dejavu.data.pluginOperationInProgress
import cafe.ivory.dejavu.data.ConsoleUiState
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.launch

@Composable
fun RootRequiredScreen(
    state: ConsoleUiState,
    onRetry: () -> Unit,
    onOpenManager: (() -> Unit)?,
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 20.dp, vertical = 16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Box(
                modifier = Modifier
                    .size(44.dp)
                    .background(
                        MaterialTheme.colorScheme.surfaceVariant,
                        androidx.compose.foundation.shape.RoundedCornerShape(12.dp),
                    ),
                contentAlignment = Alignment.Center,
            ) {
                Icon(
                    Icons.Rounded.Lock,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Column(Modifier.weight(1f)) {
                Text(
                    stringResource(R.string.root_required_title),
                    style = MaterialTheme.typography.titleLarge,
                    fontWeight = FontWeight.SemiBold,
                )
            }
        }
        Text(
            stringResource(R.string.root_required_body),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.fillMaxWidth(),
        )
        Button(
            onClick = onRetry,
            enabled = !state.loading,
            modifier = Modifier.fillMaxWidth(),
        ) {
            if (state.loading) {
                CircularProgressIndicator(
                    modifier = Modifier.size(16.dp),
                    strokeWidth = 2.dp,
                )
                Spacer(Modifier.size(8.dp))
            }
            Text(
                stringResource(
                    if (state.loading) R.string.request_root_again
                    else R.string.request_root,
                ),
            )
        }
        if (onOpenManager != null) {
            OutlinedButton(
                onClick = onOpenManager,
                modifier = Modifier.fillMaxWidth(),
            ) { Text(stringResource(R.string.open_manager)) }
        }
        Card(
            modifier = Modifier.fillMaxWidth(),
            shape = MaterialTheme.shapes.large,
            colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
            border = CardDefaults.outlinedCardBorder(),
        ) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(
                    stringResource(R.string.root_steps_title),
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.SemiBold,
                )
                RootStep(1, stringResource(R.string.root_step_1))
                RootStep(2, stringResource(R.string.root_step_2))
                RootStep(3, stringResource(R.string.root_step_3))
            }
        }
        Text(
            stringResource(R.string.root_retry_hint),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.fillMaxWidth(),
        )
        Spacer(Modifier.height(8.dp))
    }
}

@Composable
private fun RootStep(index: Int, text: String) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Box(
            modifier = Modifier
                .size(22.dp)
                .background(MaterialTheme.colorScheme.surfaceVariant, CircleShape),
            contentAlignment = Alignment.Center,
        ) {
            Text(
                index.toString(),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Text(text, style = MaterialTheme.typography.bodySmall)
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PluginsScreen(
    state: ConsoleUiState,
    pluginId: String?,
    scopeEditor: Boolean,
    onOpenPluginPage: (String) -> Unit,
    onClosePluginPage: () -> Unit,
    onShowPluginControl: () -> Unit,
    onOpenScope: (String) -> Unit,
    onSaveScope: (String, List<String>) -> Unit,
    onLoadApps: () -> Unit,
    onSetPreferredPort: (String, String, Int) -> Unit,
    onRefresh: () -> Unit,
    onSelectTarget: (String) -> Unit,
    onUnload: (String, String) -> Unit,
    onLoadLibrary: (String, String) -> Unit,
    onImportZip: () -> Unit,
    onScanDirectory: () -> Unit,
    onStartDirectory: (String, String) -> Unit,
    onStopDirectory: (String, String) -> Unit,
    onDirectoryEvents: (String, String, Int?) -> Unit,
    onCloseDirectoryEvents: () -> Unit,
    onDirectoryAction: (String, String, String) -> Unit,
    skillLoader: suspend (String, String) -> Result<String>,
    preview: Boolean = false,
) {
    val directory = state.directoryPlugins.firstOrNull { it.plugin.manifest.id == pluginId }
    val native = state.nativeLibrary.firstOrNull { it.artifact.id == pluginId }
    val busy = state.pluginOperationInProgress
    val controlsBusy = busy || state.pluginsLoading || state.pluginDirectoryLoading
    var skillText by remember(pluginId, state.selectedTarget) { mutableStateOf<String?>(null) }
    var skillLoading by remember(pluginId, state.selectedTarget) { mutableStateOf(false) }
    var pendingUnload by remember(pluginId, state.selectedTarget) { mutableStateOf<String?>(null) }
    var showingInfo by remember(pluginId) { mutableStateOf(false) }
    val coroutineScope = rememberCoroutineScope()
    val skillFallback = stringResource(R.string.msg_skill_load_failed)
    val target = state.selectedTarget.takeIf { state.activePluginId == pluginId }
    if (pluginId != null && (directory != null || native != null)) {
        val scope = state.pluginScopes[pluginId].orEmpty()
        val inScope = target != null && target in scope
        val selectedTab = if (scopeEditor) 1 else if (showingInfo) 2 else 0
        LaunchedEffect(pluginId, selectedTab) {
            if (selectedTab != 0) onCloseDirectoryEvents()
        }
        Column(Modifier.fillMaxSize()) {
            TabRow(
                selectedTabIndex = selectedTab,
                containerColor = MaterialTheme.colorScheme.background,
            ) {
                listOf(R.string.plugin_detail_control, R.string.plugin_scope_title, R.string.plugin_detail_info)
                    .forEachIndexed { index, title ->
                        Tab(
                            selected = selectedTab == index,
                            enabled = !busy,
                            onClick = {
                                if (index != 0) onCloseDirectoryEvents()
                                showingInfo = index == 2
                                if (index == 1) onOpenScope(pluginId) else onShowPluginControl()
                            },
                            text = { Text(stringResource(title), maxLines = 1, overflow = TextOverflow.Ellipsis) },
                        )
                    }
            }
            when (selectedTab) {
                1 -> Box(Modifier.weight(1f)) {
                    PluginScopeScreen(
                        id = pluginId,
                        scope = scope,
                        apps = state.installedApps,
                        loading = state.appsLoading,
                        saving = state.scopeSavingId == pluginId,
                        enabled = !busy,
                        onLoadApps = onLoadApps,
                        onSave = { onSaveScope(pluginId, it) },
                    )
                }
                2 -> PluginInformation(
                    id = pluginId,
                    name = directory?.plugin?.manifest?.name ?: native?.artifact?.name.orEmpty(),
                    version = directory?.plugin?.manifest?.version ?: native?.artifact?.version.orEmpty(),
                    type = if (directory != null) "Lua" else if (native?.artifact?.jit == true) "JIT" else "Native",
                    description = directory?.plugin?.manifest?.description ?: native?.artifact?.description.orEmpty(),
                    scope = scope,
                    modifier = Modifier.weight(1f),
                )
                else -> Column(Modifier.weight(1f)) {
                    if (state.pluginsLoading) LinearProgressIndicator(Modifier.fillMaxWidth())
                    Column(Modifier.padding(horizontal = 16.dp, vertical = 10.dp),
                        verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        TargetMenu(
                            targets = (scope + listOfNotNull(target)).distinct(),
                            selected = target,
                            onSelect = onSelectTarget,
                            enabled = !busy,
                            apps = state.installedApps,
                            label = stringResource(R.string.plugin_inspect_process),
                            modifier = Modifier.fillMaxWidth(),
                        )
                        if (scope.isEmpty()) {
                            Text(stringResource(R.string.plugin_scope_empty),
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                style = MaterialTheme.typography.bodySmall)
                        }
                        if (target != null && !inScope) ErrorBanner(stringResource(R.string.plugin_scope_removed))
                    }
                    val page = directory?.plugin?.page
                    if (directory != null && page != null) {
                        Box(Modifier.weight(1f)) {
                            PluginPageScreen(
                                page = page, plugin = directory, target = target, busy = controlsBusy,
                                working = state.pluginActionId == pluginId,
                                runtimeError = state.directoryRuntimeError,
                                events = state.packageEvents?.takeIf { it.packageId == pluginId && it.target == target },
                                onBack = onClosePluginPage, onCheck = onScanDirectory,
                                onStart = onStartDirectory, onStop = onStopDirectory,
                                onCall = onDirectoryAction, onListen = onDirectoryEvents,
                                onCloseEvents = onCloseDirectoryEvents,
                                preferredPort = target?.let { state.pluginListenPorts[pluginId to it] } ?: 0,
                                onSetPreferredPort = { port -> if (target != null) onSetPreferredPort(target, pluginId, port) },
                                targetOffline = state.directoryTargetOffline,
                                inScope = inScope,
                                hoistHeader = true,
                                preview = preview,
                            )
                        }
                    } else {
                        Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(16.dp),
                            verticalArrangement = Arrangement.spacedBy(12.dp)) {
                            state.directoryRuntimeError?.let { ErrorBanner(it) }
                            if (directory != null) {
                                StatusPill(stringResource(directory.runState.labelRes),
                                    directory.runState == cafe.ivory.dejavu.data.DirectoryPluginState.RUNNING)
                                directory.error?.let { ErrorBanner(it) }
                                val canStop = directory.runState == cafe.ivory.dejavu.data.DirectoryPluginState.RUNNING ||
                                    directory.runState == cafe.ivory.dejavu.data.DirectoryPluginState.ERROR
                                Button(onClick = {
                                    if (target != null) {
                                        if (canStop) onStopDirectory(target, pluginId)
                                        else if (directory.runState == cafe.ivory.dejavu.data.DirectoryPluginState.UNKNOWN) onScanDirectory()
                                        else onStartDirectory(target, pluginId)
                                    }
                                }, enabled = target != null && !controlsBusy && (canStop || inScope)) {
                                    Text(stringResource(when {
                                        canStop -> R.string.package_stop
                                        directory.runState == cafe.ivory.dejavu.data.DirectoryPluginState.UNKNOWN -> R.string.package_check_status
                                        else -> R.string.package_start
                                    }))
                                }
                                if (directory.plugin.manifest.events != null) {
                                    OutlinedButton(onClick = { if (target != null) onDirectoryEvents(target, pluginId, null) },
                                        enabled = inScope && !controlsBusy && directory.runState == cafe.ivory.dejavu.data.DirectoryPluginState.RUNNING && directory.session != null) {
                                        Text(stringResource(R.string.package_live_events))
                                    }
                                }
                            }
                            if (native != null) {
                                NativeLibraryPluginCard(
                                    item = native, target = target, busy = controlsBusy,
                                    working = state.nativePluginActionPath == native.artifact.path,
                                    canLoad = inScope,
                                    onLoad = { if (target != null) onLoadLibrary(target, native.artifact.path) },
                                    onUnload = { pendingUnload = native.runtime?.name },
                                    onSkill = {
                                        val runtime = native.runtime
                                        if (target != null && runtime != null) {
                                            skillLoading = true
                                            skillText = ""
                                            coroutineScope.launch {
                                                skillText = skillLoader(target, runtime.name).fold({ it }, { it.message ?: skillFallback })
                                                skillLoading = false
                                            }
                                        }
                                    },
                                )
                            }
                            OutlinedButton(onClick = onRefresh, enabled = !controlsBusy) {
                                Text(stringResource(R.string.package_check_status))
                            }
                        }
                    }
                }
            }
        }
    } else {
        val listState = rememberLazyListState()
        var showAdd by remember { mutableStateOf(true) }
        LaunchedEffect(listState) {
            var previousIndex = listState.firstVisibleItemIndex
            var previousOffset = listState.firstVisibleItemScrollOffset
            snapshotFlow { listState.firstVisibleItemIndex to listState.firstVisibleItemScrollOffset }
                .collect { (index, offset) ->
                    if (index != previousIndex || offset != previousOffset) {
                        showAdd = index < previousIndex || (index == previousIndex && offset < previousOffset)
                    }
                    previousIndex = index
                    previousOffset = offset
                }
        }
        val importing = state.pluginImporting
        val scanning = state.pluginDirectoryLoading
        val canAdd = state.rootGranted && !busy && !scanning
        Box(Modifier.fillMaxSize()) {
            LazyColumn(
                state = listState,
                modifier = Modifier.fillMaxSize(),
                contentPadding = PaddingValues(16.dp, 8.dp, 16.dp, 96.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                if (importing || scanning) {
                    item(key = "progress") {
                        Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                            LinearProgressIndicator(Modifier.fillMaxWidth())
                            Text(
                                stringResource(if (importing) R.string.package_zip_importing else R.string.package_scanning),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                    }
                }
                state.pluginDirectoryError?.let { error ->
                    item(key = "scan-error") { ErrorBanner(error) }
                }
                if (state.directoryPlugins.isEmpty() && state.nativeLibrary.isEmpty() &&
                    !importing && !scanning && state.pluginDirectoryError == null
                ) {
                    item(key = "empty") {
                        EmptyState(
                            title = stringResource(R.string.package_zip_empty_title),
                            detail = stringResource(R.string.package_zip_empty_detail),
                        )
                    }
                }
                items(state.directoryPlugins, key = { "lua:${it.plugin.manifest.id}" }) { item ->
                    val id = item.plugin.manifest.id
                    PluginSummaryCard(
                        name = item.plugin.manifest.name,
                        metadata = "Lua · ${item.plugin.manifest.version}",
                        enabled = !busy,
                        onOpen = { onCloseDirectoryEvents(); onOpenPluginPage(id) },
                    )
                }
                items(state.nativeLibrary, key = { "native:${it.artifact.path}" }) { item ->
                    val id = item.artifact.id
                    if (id != null) PluginSummaryCard(
                        name = item.artifact.name,
                        metadata = (if (item.artifact.jit) "JIT" else "Native") + " · " + item.artifact.version,
                        enabled = !busy,
                        onOpen = { onOpenPluginPage(id) },
                    )
                }
            }
            AnimatedVisibility(
                visible = showAdd || importing || !listState.canScrollBackward || !listState.canScrollForward,
                modifier = Modifier.align(Alignment.BottomEnd).padding(16.dp),
                enter = fadeIn() + slideInVertically { it },
                exit = fadeOut() + slideOutVertically { it },
            ) {
                FloatingActionButton(
                    onClick = { if (canAdd) onImportZip() },
                    modifier = Modifier.semantics { if (!canAdd) disabled() },
                    containerColor = if (canAdd || importing) MaterialTheme.colorScheme.primaryContainer
                        else MaterialTheme.colorScheme.surfaceVariant,
                    contentColor = if (canAdd || importing) MaterialTheme.colorScheme.onPrimaryContainer
                        else MaterialTheme.colorScheme.onSurfaceVariant,
                ) {
                    if (importing) {
                        CircularProgressIndicator(Modifier.size(24.dp), strokeWidth = 2.dp)
                    } else {
                        Icon(Icons.Rounded.Add, contentDescription = stringResource(R.string.package_add_zip))
                    }
                }
            }
        }
    }
    if (pendingUnload != null) {
        AlertDialog(onDismissRequest = { pendingUnload = null },
            title = { Text(stringResource(R.string.dialog_unload_plugin_title)) },
            text = { Text(stringResource(R.string.dialog_unload_plugin_body, pendingUnload.orEmpty())) },
            confirmButton = { TextButton(enabled = !busy, onClick = {
                val name = pendingUnload
                if (target != null && name != null) onUnload(target, name)
                pendingUnload = null
            }) { Text(stringResource(R.string.unload)) } },
            dismissButton = { TextButton(onClick = { pendingUnload = null }) { Text(stringResource(R.string.cancel)) } })
    }
    if (skillText != null) {
        ModalBottomSheet(onDismissRequest = { skillText = null }) {
            Column(Modifier.fillMaxWidth().padding(20.dp).verticalScroll(rememberScrollState())) {
                Text(stringResource(R.string.skill_md), style = MaterialTheme.typography.titleLarge)
                if (skillLoading) CircularProgressIndicator()
                else SelectionContainer { Text(skillText.orEmpty(), style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace)) }
                Spacer(Modifier.height(32.dp))
            }
        }
    }
    if (!preview && !scopeEditor && !showingInfo && directory?.plugin?.page == null) state.packageEvents?.takeIf { it.packageId == pluginId && it.target == target }?.let {
        DirectoryPluginEventsSheet(it, onCloseDirectoryEvents)
    }
}

@Composable
private fun PluginInformation(
    id: String,
    name: String,
    version: String,
    type: String,
    description: String,
    scope: List<String>,
    modifier: Modifier = Modifier,
) {
    SelectionContainer(modifier.fillMaxWidth()) {
        LazyColumn(
            modifier = Modifier.fillMaxSize(),
            contentPadding = PaddingValues(16.dp, 16.dp, 16.dp, 24.dp),
            verticalArrangement = Arrangement.spacedBy(20.dp),
        ) {
            item {
                Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text(name, style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.SemiBold)
                    Text("$type · $version", style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
            item {
                Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text(stringResource(R.string.package_plugin_id), style = MaterialTheme.typography.labelLarge,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Text(id, style = MaterialTheme.typography.bodyMedium.copy(fontFamily = FontFamily.Monospace))
                }
            }
            item {
                Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text(stringResource(R.string.plugin_detail_description), style = MaterialTheme.typography.labelLarge,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Text(if (description.isBlank()) stringResource(R.string.package_plugin_no_description) else description,
                        style = MaterialTheme.typography.bodyMedium)
                }
            }
            item {
                Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text(stringResource(R.string.plugin_scope_count, scope.size), style = MaterialTheme.typography.labelLarge,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                    if (scope.isEmpty()) Text(stringResource(R.string.plugin_detail_no_scope),
                        style = MaterialTheme.typography.bodyMedium)
                }
            }
            items(scope, key = { it }) { process ->
                Text(process, style = MaterialTheme.typography.bodyMedium.copy(fontFamily = FontFamily.Monospace))
            }
        }
    }
}

@Composable
fun LogsScreen(
    state: ConsoleUiState,
    onStart: () -> Unit,
    onStop: () -> Unit,
    onClear: () -> Unit,
) {
    var filter by rememberSaveable { mutableStateOf("") }
    var autoScroll by rememberSaveable { mutableStateOf(true) }
    val lines = remember(state.logText, filter) {
        state.logText.lineSequence()
            .filter { it.isNotBlank() }
            .filter { filter.isBlank() || it.contains(filter, ignoreCase = true) }
            .toList()
    }
    val listState = rememberLazyListState()
    LaunchedEffect(lines.size, autoScroll) {
        if (autoScroll && lines.isNotEmpty()) listState.scrollToItem(lines.lastIndex)
    }
    Column(Modifier.fillMaxSize()) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 8.dp),
            horizontalArrangement = Arrangement.spacedBy(4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            OutlinedTextField(
                value = filter,
                onValueChange = { filter = it },
                modifier = Modifier.weight(1f),
                leadingIcon = { Icon(Icons.Rounded.Search, contentDescription = null) },
                placeholder = { Text(stringResource(R.string.logs_filter)) },
                singleLine = true,
                shape = MaterialTheme.shapes.medium,
            )
            IconButton(onClick = if (state.logcatRunning) onStop else onStart) {
                Icon(
                    if (state.logcatRunning) Icons.Rounded.Stop else Icons.Rounded.PlayArrow,
                    contentDescription = if (state.logcatRunning) {
                        stringResource(R.string.logs_stop)
                    } else {
                        stringResource(R.string.logs_start)
                    },
                )
            }
            IconButton(onClick = { autoScroll = !autoScroll }) {
                Icon(
                    Icons.Rounded.VerticalAlignBottom,
                    contentDescription = stringResource(R.string.logs_auto_scroll),
                    tint = if (autoScroll) MaterialTheme.colorScheme.primary
                    else MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            IconButton(onClick = onClear) {
                Icon(
                    Icons.Rounded.DeleteOutline,
                    contentDescription = stringResource(R.string.logs_clear),
                )
            }
        }
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 0.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                stringResource(R.string.logs_lines, lines.size),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.weight(1f),
            )
            if (state.logcatRunning) {
                StatusPill(stringResource(R.string.status_running), true)
            }
        }
        if (lines.isEmpty()) {
            EmptyState(
                title = if (filter.isNotBlank()) {
                    stringResource(R.string.logs_no_matches)
                } else if (state.logcatRunning) {
                    stringResource(R.string.logs_empty_running_title)
                } else {
                    stringResource(R.string.logs_empty_title)
                },
                detail = if (filter.isNotBlank()) {
                    stringResource(R.string.logs_filter_empty_detail)
                } else {
                    stringResource(R.string.logs_empty_detail)
                },
                icon = { Icon(Icons.AutoMirrored.Rounded.Article, contentDescription = null) },
            )
        } else {
            SelectionContainer(Modifier.weight(1f)) {
                LazyColumn(
                    modifier = Modifier.fillMaxSize(),
                    state = listState,
                    contentPadding = PaddingValues(horizontal = 16.dp, vertical = 4.dp),
                ) {
                    itemsIndexed(lines) { _, line ->
                        Text(
                            line,
                            style = MaterialTheme.typography.bodySmall.copy(
                                fontFamily = FontFamily.Monospace,
                            ),
                            color = when {
                                line.contains(" E ") || line.contains("FATAL") ->
                                    MaterialTheme.colorScheme.error
                                line.contains(" W ") ->
                                    MaterialTheme.colorScheme.tertiary
                                else -> MaterialTheme.colorScheme.onSurfaceVariant
                            },
                        )
                    }
                }
            }
        }
    }
}
