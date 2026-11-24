package cafe.ivory.dejavu.ui

import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.expandHorizontally
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkHorizontally
import androidx.compose.animation.core.tween
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.interaction.MutableInteractionSource
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
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import cafe.ivory.dejavu.R
import cafe.ivory.dejavu.data.DirectoryPluginState
import cafe.ivory.dejavu.data.EnvironmentUiState
import cafe.ivory.dejavu.data.LibraryPluginItem
import cafe.ivory.dejavu.data.PluginDetailTab
import cafe.ivory.dejavu.data.PluginsUiState
import cafe.ivory.dejavu.data.operationInProgress
import cafe.ivory.dejavu.ui.theme.mono

@Immutable
internal data class PluginDetailActions(
    val onSelectTab: (PluginDetailTab) -> Unit,
    val onClose: () -> Unit,
    val onSaveScope: (String, List<String>) -> Unit,
    val onLoadApps: () -> Unit,
    val onSetPreferredPort: (String, String, Int) -> Unit,
    val onRefreshRuntime: () -> Unit,
    val onSelectTarget: (String) -> Unit,
    val onUnloadNative: (String, String) -> Unit,
    val onLoadNative: (String, String) -> Unit,
    val onStartDirectory: (String, String) -> Unit,
    val onStopDirectory: (String, String) -> Unit,
    val onOpenEvents: (String, String, Int?) -> Unit,
    val onCloseEvents: () -> Unit,
    val onRunAction: (String, String, String) -> Unit,
    val onLoadSkill: (String, String) -> Unit,
    val onDismissSkill: () -> Unit,
)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun PluginDetailScreen(
    pluginId: String,
    activeTab: PluginDetailTab,
    envState: EnvironmentUiState,
    pluginsState: PluginsUiState,
    actions: PluginDetailActions,
    onScopeDirtyChange: (Boolean) -> Unit,
    pendingLeaveConfirmation: Boolean,
    onResolveLeaveConfirmation: (save: Boolean?) -> Unit,
    preview: Boolean = false,
) {
    val item = remember(pluginId, pluginsState.directoryPlugins, pluginsState.nativeLibrary) {
        pluginsState.findItem(pluginId)
    }
    val directory = (item as? LibraryPluginItem.Lua)?.ui
    val native = (item as? LibraryPluginItem.Native)?.ui
    val busy = pluginsState.operationInProgress(envState.scopeSavingId)
    val controlsBusy = busy || pluginsState.pluginsLoading || pluginsState.pluginDirectoryLoading
    val scope = envState.pluginScopes[pluginId].orEmpty()
    val target = pluginsState.selectedTarget.takeIf { pluginsState.activePluginId == pluginId }
    val inScope = target != null && target in scope
    val appsByPackage = remember(envState.installedApps) {
        envState.installedApps.associateBy { it.packageName }
    }

    // Lift scope draft to detail level so switching tabs doesn't silently discard unsaved selections.
    var draftSelected by rememberSaveable(pluginId, scope) { mutableStateOf(scope) }
    var draftCustomTargets by rememberSaveable(pluginId, scope) { mutableStateOf(emptyList<String>()) }
    val scopeDirty = remember(draftSelected, scope) { draftSelected.toSet() != scope.toSet() }
    LaunchedEffect(scopeDirty) { onScopeDirtyChange(scopeDirty) }

    var pendingTabSwitch by remember { mutableStateOf<PluginDetailTab?>(null) }
    var pendingUnload by remember(pluginId, target) { mutableStateOf<String?>(null) }

    // If a background library rescan removes this plugin, leave detail cleanly instead of stranding UI.
    LaunchedEffect(item, pluginsState.pluginDirectoryLoading, preview) {
        if (!preview && item == null && !pluginsState.pluginDirectoryLoading) {
            actions.onClose()
        }
    }

    LaunchedEffect(pluginId, activeTab, preview) {
        if (!preview && activeTab != PluginDetailTab.Control) actions.onCloseEvents()
    }

    if (item == null) {
        Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            CircularProgressIndicator()
        }
        return
    }

    val isLtr = LocalLayoutDirection.current == LayoutDirection.Ltr
    Column(Modifier.fillMaxSize().imePadding()) {
        Column(
            Modifier.padding(start = 16.dp, end = 16.dp, top = 4.dp, bottom = 8.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            TargetMenu(
                targets = (scope + listOfNotNull(target)).distinct(),
                selected = target,
                onSelect = actions.onSelectTarget,
                enabled = !busy,
                appsByPackage = appsByPackage,
                label = stringResource(R.string.plugin_inspect_process),
                modifier = Modifier.fillMaxWidth(),
            )
            SlidingSegmented(
                options = listOf(
                    stringResource(R.string.plugin_detail_control),
                    stringResource(R.string.plugin_scope_title),
                    stringResource(R.string.plugin_detail_info),
                ),
                selected = activeTab.ordinal,
                enabled = !busy,
                onSelect = { index ->
                    val nextTab = PluginDetailTab.entries[index]
                    if (activeTab == PluginDetailTab.Scope && nextTab != PluginDetailTab.Scope && scopeDirty) {
                        pendingTabSwitch = nextTab
                    } else {
                        if (nextTab != PluginDetailTab.Control) actions.onCloseEvents()
                        actions.onSelectTab(nextTab)
                    }
                },
            )
        }
        Box(Modifier.fillMaxWidth().height(4.dp)) {
            androidx.compose.animation.AnimatedVisibility(
                visible = activeTab == PluginDetailTab.Control && pluginsState.pluginsLoading,
                enter = fadeIn(tween(Motion.FADE_IN)),
                exit = fadeOut(tween(Motion.FADE_OUT)),
            ) {
                LinearProgressIndicator(Modifier.fillMaxWidth())
            }
        }
        AnimatedContent(
            targetState = activeTab,
            modifier = Modifier.weight(1f),
            transitionSpec = { Motion.tab(if (targetState.ordinal > initialState.ordinal) 1 else -1, isLtr) },
            label = "detail-tab",
        ) { tab ->
            Column(Modifier.fillMaxSize()) {
                when (tab) {
                    PluginDetailTab.Scope -> PluginScopeScreen(
                        id = pluginId,
                        savedScope = scope,
                        selected = draftSelected,
                        customTargets = draftCustomTargets,
                        apps = envState.installedApps,
                        appsByPackage = appsByPackage,
                        loading = envState.appsLoading,
                        saving = envState.scopeSavingId == pluginId,
                        enabled = !busy,
                        onSelectedChange = { draftSelected = it },
                        onAddCustomTarget = { process ->
                            if (process !in draftCustomTargets && process !in scope) {
                                draftCustomTargets = listOf(process) + draftCustomTargets
                            }
                            if (process !in draftSelected) {
                                draftSelected = (listOf(process) + draftSelected).distinct()
                            }
                        },
                        onLoadApps = actions.onLoadApps,
                        onSave = { actions.onSaveScope(pluginId, it) },
                    )

                    PluginDetailTab.Info -> PluginInformation(
                        id = item.id,
                        name = item.name,
                        version = item.version,
                        type = item.kindTag,
                        description = item.description,
                        scope = scope,
                        modifier = Modifier.weight(1f),
                    )

                    PluginDetailTab.Control -> {
                        if (scope.isEmpty() || (target != null && !inScope)) {
                            Column(
                                Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
                                verticalArrangement = Arrangement.spacedBy(8.dp),
                            ) {
                                if (scope.isEmpty()) {
                                    NoticeBanner(stringResource(R.string.plugin_scope_empty))
                                    FilledTonalButton(
                                        onClick = { actions.onSelectTab(PluginDetailTab.Scope) },
                                        enabled = !busy,
                                        modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
                                    ) {
                                        Text(stringResource(R.string.plugin_go_to_scope))
                                    }
                                }
                                if (target != null && !inScope) {
                                    ErrorBanner(stringResource(R.string.plugin_scope_removed))
                                }
                            }
                        }
                        val page = directory?.plugin?.page
                        if (directory != null && page != null) {
                            Box(Modifier.weight(1f)) {
                                PluginPageScreen(
                                    page = page,
                                    plugin = directory,
                                    target = target,
                                    busy = controlsBusy,
                                    working = pluginsState.pluginActionId == pluginId,
                                    runtimeError = pluginsState.directoryRuntimeError,
                                    events = pluginsState.packageEvents?.takeIf {
                                        it.packageId == pluginId && it.target == target
                                    },
                                    onBack = actions.onClose,
                                    onCheck = actions.onRefreshRuntime,
                                    onStart = actions.onStartDirectory,
                                    onStop = actions.onStopDirectory,
                                    onCall = actions.onRunAction,
                                    onListen = actions.onOpenEvents,
                                    onCloseEvents = actions.onCloseEvents,
                                    preferredPort = target?.let { pluginsState.pluginListenPorts[pluginId to it] } ?: 0,
                                    onSetPreferredPort = { port ->
                                        if (target != null) actions.onSetPreferredPort(target, pluginId, port)
                                    },
                                    targetOffline = pluginsState.directoryTargetOffline,
                                    inScope = inScope,
                                    hoistHeader = true,
                                    preview = preview || tab != activeTab,
                                )
                            }
                        } else {
                            Column(
                                Modifier
                                    .weight(1f)
                                    .verticalScroll(rememberScrollState())
                                    .padding(16.dp),
                                verticalArrangement = Arrangement.spacedBy(12.dp),
                            ) {
                                pluginsState.directoryRuntimeError?.let {
                                    if (pluginsState.directoryTargetOffline) NoticeBanner(it) else ErrorBanner(it)
                                }
                                if (directory != null) {
                                    RuntimeStateBlock(
                                        label = stringResource(directory.runState.labelRes),
                                        live = directory.runState == DirectoryPluginState.RUNNING,
                                        error = directory.runState == DirectoryPluginState.ERROR,
                                        session = directory.session.takeIf {
                                            directory.runState == DirectoryPluginState.RUNNING ||
                                                directory.runState == DirectoryPluginState.ERROR
                                        },
                                        hint = if (directory.runState == DirectoryPluginState.UNKNOWN) {
                                            stringResource(R.string.detail_runtime_hint_unknown)
                                        } else null,
                                    )
                                    directory.error?.let { ErrorBanner(it) }
                                    if (directory.plugin.manifest.events != null) {
                                        val canEvents = inScope && !controlsBusy &&
                                            directory.runState == DirectoryPluginState.RUNNING &&
                                            directory.session != null
                                        if (directory.runState != DirectoryPluginState.UNKNOWN) {
                                            OutlinedButton(
                                                onClick = { if (target != null) actions.onOpenEvents(target, pluginId, null) },
                                                modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
                                                enabled = canEvents,
                                            ) {
                                                Text(stringResource(R.string.package_live_events))
                                            }
                                        }
                                    }
                                    if (directory.runState != DirectoryPluginState.UNKNOWN) {
                                        TextButton(
                                            onClick = actions.onRefreshRuntime,
                                            enabled = !controlsBusy,
                                            modifier = Modifier.heightIn(min = 48.dp),
                                        ) {
                                            Text(stringResource(R.string.package_check_status))
                                        }
                                    }
                                }
                                if (native != null) {
                                    val loaded = native.runtime?.loaded == true
                                    RuntimeStateBlock(
                                        label = stringResource(
                                            when {
                                                !native.runtimeKnown -> R.string.package_state_unknown
                                                loaded -> R.string.status_loaded
                                                else -> R.string.native_library_not_loaded
                                            },
                                        ),
                                        live = loaded,
                                        error = native.error != null,
                                        session = null,
                                        hint = if (!native.runtimeKnown) {
                                            stringResource(R.string.detail_runtime_hint_unknown)
                                        } else null,
                                    )
                                    native.error?.let { ErrorBanner(it) }
                                    NativeLibraryMetadataCard(
                                        item = native,
                                        target = target,
                                        busy = controlsBusy,
                                        onSkill = {
                                            val runtime = native.runtime
                                            if (target != null && runtime != null) {
                                                actions.onLoadSkill(target, runtime.name)
                                            }
                                        },
                                    )
                                    if (native.runtimeKnown) {
                                        TextButton(
                                            onClick = actions.onRefreshRuntime,
                                            enabled = !controlsBusy,
                                            modifier = Modifier.heightIn(min = 48.dp),
                                        ) {
                                            Text(stringResource(R.string.package_check_status))
                                        }
                                    }
                                }
                            }
                            if (directory != null) {
                                val runState = directory.runState
                                val canStop = runState == DirectoryPluginState.RUNNING ||
                                    runState == DirectoryPluginState.ERROR
                                val unknown = runState == DirectoryPluginState.UNKNOWN
                                val working = pluginsState.pluginActionId == pluginId ||
                                    (unknown && pluginsState.pluginsLoading)
                                DockedPrimaryAction(
                                    destructive = canStop,
                                    label = stringResource(
                                        when {
                                            unknown && pluginsState.pluginsLoading -> R.string.package_checking_status
                                            canStop -> R.string.package_stop
                                            unknown -> R.string.package_check_status
                                            else -> R.string.package_start
                                        },
                                    ),
                                    working = working,
                                    enabled = target != null && !controlsBusy && (canStop || inScope),
                                    onClick = {
                                        if (target != null) {
                                            when {
                                                canStop -> actions.onStopDirectory(target, pluginId)
                                                unknown -> actions.onRefreshRuntime()
                                                else -> actions.onStartDirectory(target, pluginId)
                                            }
                                        }
                                    },
                                )
                            } else if (native != null) {
                                val loaded = native.runtime?.loaded == true
                                val unknown = !native.runtimeKnown
                                val working = pluginsState.nativePluginActionPath == native.artifact.path ||
                                    (unknown && pluginsState.pluginsLoading)
                                DockedPrimaryAction(
                                    destructive = loaded,
                                    label = stringResource(
                                        when {
                                            unknown && pluginsState.pluginsLoading -> R.string.package_checking_status
                                            loaded -> R.string.unload
                                            unknown -> R.string.package_check_status
                                            else -> R.string.load
                                        },
                                    ),
                                    working = working,
                                    enabled = target != null && !controlsBusy && (loaded || inScope),
                                    onClick = {
                                        if (target != null) {
                                            when {
                                                loaded -> pendingUnload = native.runtime?.name
                                                unknown -> actions.onRefreshRuntime()
                                                else -> actions.onLoadNative(target, native.artifact.path)
                                            }
                                        }
                                    },
                                )
                            }
                        }
                    }
                }
            }
        }
    }

    if (pendingUnload != null) {
        AlertDialog(
            onDismissRequest = { pendingUnload = null },
            title = { Text(stringResource(R.string.dialog_unload_plugin_title)) },
            text = { Text(stringResource(R.string.dialog_unload_plugin_body, pendingUnload.orEmpty())) },
            confirmButton = {
                TextButton(
                    enabled = !busy,
                    onClick = {
                        val name = pendingUnload
                        if (target != null && name != null) actions.onUnloadNative(target, name)
                        pendingUnload = null
                    },
                ) { Text(stringResource(R.string.unload)) }
            },
            dismissButton = {
                TextButton(onClick = { pendingUnload = null }) { Text(stringResource(R.string.cancel)) }
            },
        )
    }

    pluginsState.skillSheet?.let { sheet ->
        ModalBottomSheet(onDismissRequest = actions.onDismissSkill) {
            Column(
                Modifier
                    .fillMaxWidth()
                    .padding(20.dp)
                    .verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Text(stringResource(R.string.skill_md), style = MaterialTheme.typography.titleLarge)
                if (sheet.loading) {
                    CircularProgressIndicator()
                } else {
                    SelectionContainer {
                        Text(sheet.text, style = MaterialTheme.typography.mono)
                    }
                }
                Spacer(Modifier.height(32.dp))
            }
        }
    }

    val confirmUnsaved = pendingTabSwitch != null || pendingLeaveConfirmation
    if (confirmUnsaved) {
        AlertDialog(
            onDismissRequest = {
                pendingTabSwitch = null
                onResolveLeaveConfirmation(null)
            },
            title = { Text(stringResource(R.string.plugin_scope_unsaved_title)) },
            text = { Text(stringResource(R.string.plugin_scope_unsaved_body)) },
            confirmButton = {
                TextButton(
                    onClick = {
                        actions.onSaveScope(pluginId, draftSelected.distinct().sorted())
                        val next = pendingTabSwitch
                        pendingTabSwitch = null
                        if (next != null) {
                            if (next != PluginDetailTab.Control) actions.onCloseEvents()
                            actions.onSelectTab(next)
                        } else {
                            onResolveLeaveConfirmation(true)
                        }
                    },
                ) { Text(stringResource(R.string.plugin_scope_unsaved_save)) }
            },
            dismissButton = {
                TextButton(
                    onClick = {
                        draftSelected = scope
                        draftCustomTargets = emptyList()
                        val next = pendingTabSwitch
                        pendingTabSwitch = null
                        if (next != null) {
                            if (next != PluginDetailTab.Control) actions.onCloseEvents()
                            actions.onSelectTab(next)
                        } else {
                            onResolveLeaveConfirmation(false)
                        }
                    },
                ) { Text(stringResource(R.string.plugin_scope_unsaved_discard)) }
            },
        )
    }

    if (!preview && activeTab == PluginDetailTab.Control && directory?.plugin?.page == null) {
        pluginsState.packageEvents?.takeIf { it.packageId == pluginId && it.target == target }?.let {
            DirectoryPluginEventsSheet(it, actions.onCloseEvents)
        }
    }
}

@Composable
private fun DockedPrimaryAction(
    destructive: Boolean,
    label: String,
    working: Boolean,
    enabled: Boolean,
    onClick: () -> Unit,
) {
    Column {
        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
        Box(
            Modifier
                .fillMaxWidth()
                .background(MaterialTheme.colorScheme.surface)
                .padding(horizontal = 16.dp, vertical = 12.dp),
        ) {
            val press = remember(destructive) { MutableInteractionSource() }
            val modifier = Modifier.fillMaxWidth().heightIn(min = 52.dp).pressScale(press)
            val content: @Composable androidx.compose.foundation.layout.RowScope.() -> Unit = {
                AnimatedVisibility(
                    visible = working,
                    enter = fadeIn() + expandHorizontally(),
                    exit = fadeOut() + shrinkHorizontally(),
                ) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        CircularProgressIndicator(
                            Modifier.size(18.dp),
                            strokeWidth = 2.dp,
                            color = LocalContentColor.current,
                        )
                        Spacer(Modifier.size(10.dp))
                    }
                }
                AnimatedContent(
                    targetState = label,
                    transitionSpec = { Motion.roll() },
                    label = "primary-label",
                ) { text ->
                    Text(text)
                }
            }
            if (destructive) {
                OutlinedButton(
                    onClick = onClick,
                    enabled = enabled,
                    modifier = modifier,
                    interactionSource = press,
                    colors = ButtonDefaults.outlinedButtonColors(contentColor = MaterialTheme.colorScheme.error),
                    border = BorderStroke(
                        1.dp,
                        if (enabled) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.outlineVariant,
                    ),
                    content = content,
                )
            } else {
                Button(
                    onClick = onClick,
                    enabled = enabled,
                    modifier = modifier,
                    interactionSource = press,
                    content = content,
                )
            }
        }
    }
}

@Composable
private fun RuntimeStateBlock(
    label: String,
    live: Boolean,
    error: Boolean,
    session: String?,
    hint: String?,
) {
    ManagerGroup {
        Column(
            Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 18.dp)
                .semantics(mergeDescendants = true) {},
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            Text(
                stringResource(R.string.plugin_detail_runtime),
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                StatusDot(live = live, error = error)
                AnimatedContent(
                    targetState = label to error,
                    transitionSpec = { Motion.roll() },
                    label = "run-state",
                ) { (shownLabel, isError) ->
                    Text(
                        shownLabel,
                        style = MaterialTheme.typography.headlineSmall,
                        color = if (isError) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface,
                    )
                }
            }
            if (session != null) {
                Text(
                    stringResource(R.string.detail_session, session),
                    style = MaterialTheme.typography.mono,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            } else if (hint != null) {
                Text(
                    hint,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
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
                    TypeTag(if (version.isBlank()) type else "$type · $version")
                }
            }
            item {
                Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text(
                        stringResource(R.string.package_plugin_id),
                        style = MaterialTheme.typography.labelLarge,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Text(id, style = MaterialTheme.typography.mono.copy(fontSize = MaterialTheme.typography.bodyMedium.fontSize))
                }
            }
            item {
                Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text(
                        stringResource(R.string.plugin_detail_description),
                        style = MaterialTheme.typography.labelLarge,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Text(
                        if (description.isBlank()) stringResource(R.string.package_plugin_no_description) else description,
                        style = MaterialTheme.typography.bodyMedium,
                    )
                }
            }
            item {
                Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text(
                        pluralStringResource(R.plurals.plugin_scope_count, scope.size, scope.size),
                        style = MaterialTheme.typography.labelLarge,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    if (scope.isEmpty()) {
                        Text(
                            stringResource(R.string.plugin_detail_no_scope),
                            style = MaterialTheme.typography.bodyMedium,
                        )
                    }
                }
            }
            items(scope, key = { it }) { process ->
                Text(
                    process,
                    style = MaterialTheme.typography.mono.copy(fontSize = MaterialTheme.typography.bodyMedium.fontSize),
                )
            }
        }
    }
}
