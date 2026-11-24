package cafe.ivory.dejavu.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.KeyboardArrowRight
import androidx.compose.material.icons.rounded.Close
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import cafe.ivory.dejavu.R
import cafe.ivory.dejavu.data.EnvironmentUiState
import cafe.ivory.dejavu.data.InstalledApp
import cafe.ivory.dejavu.data.LibraryPluginItem
import cafe.ivory.dejavu.data.ModuleUiState
import cafe.ivory.dejavu.ui.theme.mono
import kotlinx.coroutines.delay

private data class ScopedPlugin(val id: String, val name: String?, val type: String?)
private data class ProcessGroup(val process: String, val plugins: List<ScopedPlugin>)

@Composable
internal fun OverviewScreen(
    envState: EnvironmentUiState,
    moduleState: ModuleUiState,
    libraryItems: List<LibraryPluginItem>,
    onManageModule: () -> Unit,
    onManagePlugins: () -> Unit,
    onRefresh: () -> Unit,
    onOpenPlugin: (id: String, process: String) -> Unit,
    onRemoveManualTarget: (String) -> Unit,
    preview: Boolean = false,
) {
    val appsByPackage = remember(envState.installedApps) {
        envState.installedApps.associateBy { it.packageName }
    }
    val groups = remember(envState.pluginScopes, libraryItems) { processGroups(envState.pluginScopes, libraryItems) }
    val distinctPluginCount = remember(groups) { groups.flatMap { it.plugins }.distinctBy { it.id }.size }
    val manual = remember(envState.targets, groups) {
        val scoped = groups.map { it.process }.toSet()
        envState.targets.filter { it !in scoped }.distinct()
    }
    val revealMemory = LocalRevealMemory.current
    val readyForReveal = !preview && !envState.loading
    var revealActive by remember { mutableStateOf(false) }
    LaunchedEffect(readyForReveal) {
        if (readyForReveal && revealMemory.consumeFirstTime("overview")) {
            revealActive = true
            delay(550)
            revealActive = false
        }
    }
    val listState = rememberLazyListState()
    LaunchedEffect(listState.isScrollInProgress) {
        if (listState.isScrollInProgress && revealActive) revealActive = false
    }
    var pendingRemoveManual by remember { mutableStateOf<String?>(null) }

    LazyColumn(
        state = listState,
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(start = 16.dp, top = 4.dp, end = 16.dp, bottom = 24.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item(key = "chain") {
            Reveal(0, revealActive) {
                OverviewChain(envState, moduleState, onManageModule, onRefresh)
            }
        }
        item(key = "injecting-title") {
            Reveal(1, revealActive) {
                Row(
                    Modifier.fillMaxWidth().padding(top = 8.dp, start = 4.dp, end = 4.dp),
                    verticalAlignment = Alignment.Bottom,
                ) {
                    Text(
                        stringResource(R.string.injecting_title),
                        style = MaterialTheme.typography.titleMedium,
                        modifier = Modifier.weight(1f),
                    )
                    if (groups.isNotEmpty()) {
                        val procCount = pluralStringResource(R.plurals.count_processes, groups.size, groups.size)
                        val plugCount = pluralStringResource(R.plurals.count_plugins, distinctPluginCount, distinctPluginCount)
                        Text(
                            "$procCount · $plugCount",
                            style = MaterialTheme.typography.labelMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }
        }
        if (groups.isEmpty() && !envState.loading) {
            item(key = "injecting-empty") {
                Box(Modifier.animateItem()) {
                    Reveal(2, revealActive) {
                        ManagerGroup {
                            Column(
                                Modifier.fillMaxWidth().padding(20.dp),
                                verticalArrangement = Arrangement.spacedBy(8.dp),
                                horizontalAlignment = Alignment.CenterHorizontally,
                            ) {
                                ManagerMark(Modifier.size(72.dp))
                                Text(stringResource(R.string.injecting_empty_title), style = MaterialTheme.typography.titleSmall)
                                Text(
                                    stringResource(R.string.injecting_empty_detail),
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                    textAlign = TextAlign.Center,
                                )
                                FilledTonalButton(onClick = onManagePlugins, modifier = Modifier.heightIn(min = 48.dp)) {
                                    Text(stringResource(R.string.injecting_open_plugins))
                                }
                            }
                        }
                    }
                }
            }
        }
        itemsIndexed(groups, key = { _, it -> "process:${it.process}" }) { index, group ->
            Box(Modifier.animateItem()) {
                Reveal(2 + index, revealActive) {
                    ProcessGroupCard(group, appsByPackage, onOpenPlugin)
                }
            }
        }
        if (manual.isNotEmpty()) {
            item(key = "manual") {
                Box(Modifier.animateItem()) {
                    Reveal(2 + groups.size, revealActive) {
                        ManualTargets(
                            targets = manual,
                            appsByPackage = appsByPackage,
                            busy = envState.scopeSavingId != null || envState.manualRemovingTarget != null,
                            onRequestRemove = { pendingRemoveManual = it },
                        )
                    }
                }
            }
        }
    }

    pendingRemoveManual?.let { target ->
        AlertDialog(
            onDismissRequest = { pendingRemoveManual = null },
            title = { Text(stringResource(R.string.manual_target_remove_title)) },
            text = { Text(stringResource(R.string.manual_target_remove_body, target)) },
            confirmButton = {
                TextButton(
                    onClick = {
                        pendingRemoveManual = null
                        onRemoveManualTarget(target)
                    },
                ) { Text(stringResource(R.string.manual_target_remove_confirm)) }
            },
            dismissButton = {
                TextButton(onClick = { pendingRemoveManual = null }) { Text(stringResource(R.string.cancel)) }
            },
        )
    }
}

private fun processGroups(
    pluginScopes: Map<String, List<String>>,
    libraryItems: List<LibraryPluginItem>,
): List<ProcessGroup> {
    val byId = libraryItems.associateBy { it.id }
    val byProcess = linkedMapOf<String, MutableList<ScopedPlugin>>()
    pluginScopes.toSortedMap().forEach { (id, processes) ->
        val item = byId[id]
        val plugin = if (item != null) ScopedPlugin(id, item.name, item.kindTag) else ScopedPlugin(id, null, null)
        processes.distinct().forEach { byProcess.getOrPut(it) { mutableListOf() }.add(plugin) }
    }
    return byProcess.entries.sortedBy { it.key }.map { ProcessGroup(it.key, it.value) }
}

@Composable
private fun OverviewChain(
    envState: EnvironmentUiState,
    moduleState: ModuleUiState,
    onManageModule: () -> Unit,
    onRefresh: () -> Unit,
) {
    val snapshot = moduleState.snapshot
    val installedVersion = snapshot?.installedVersion
    val installed = !installedVersion.isNullOrBlank()
    val moduleNodeState = when {
        moduleState.working -> ChainNodeState.Pending
        snapshot?.removalPending == true -> ChainNodeState.Broken
        snapshot?.disabled == true -> ChainNodeState.Broken
        snapshot?.rebootPending == true -> ChainNodeState.Pending
        installed -> ChainNodeState.Healthy
        moduleState.loading -> ChainNodeState.Pending
        else -> ChainNodeState.Broken
    }
    val moduleDetail = when {
        moduleState.working -> stringResource(R.string.module_install_progress)
        snapshot?.removalPending == true -> stringResource(R.string.module_install_removing)
        snapshot?.rebootPending == true -> stringResource(R.string.module_install_pending)
        snapshot?.disabled == true -> stringResource(R.string.module_install_disabled)
        installed -> installedVersion
        moduleState.loading -> stringResource(R.string.module_install_checking)
        else -> stringResource(R.string.chain_missing)
    }
    val moduleAction = when {
        snapshot?.rebootPending == true -> stringResource(R.string.chain_reboot_pending_action)
        installed -> stringResource(R.string.chain_open_module)
        else -> stringResource(R.string.chain_install_module)
    }
    val helperDetail = if (envState.helperRunning) {
        envState.helperPort.takeIf { it.isNotBlank() }?.let { ":$it" } ?: stringResource(R.string.overview_running)
    } else stringResource(R.string.overview_offline)

    InjectionChain(
        links = listOf(
            ChainLink(
                label = stringResource(R.string.chain_module),
                detail = moduleDetail,
                state = moduleNodeState,
                actionLabel = moduleAction,
                onAction = onManageModule,
                onClick = onManageModule,
            ),
            ChainLink(
                label = stringResource(R.string.chain_root),
                detail = stringResource(R.string.overview_root_granted),
                state = ChainNodeState.Healthy,
            ),
            ChainLink(
                label = stringResource(R.string.chain_helper),
                detail = helperDetail,
                state = if (envState.helperRunning) ChainNodeState.Healthy else ChainNodeState.Broken,
                actionLabel = stringResource(R.string.chain_recheck_helper),
                onAction = onRefresh,
            ),
        ),
        refreshing = envState.loading || moduleState.loading,
    )
}

@Composable
private fun ProcessGroupCard(
    group: ProcessGroup,
    appsByPackage: Map<String, InstalledApp>,
    onOpenPlugin: (String, String) -> Unit,
) {
    val pkg = group.process.substringBefore(':')
    val app = appsByPackage[pkg]
    ManagerGroup {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 12.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            ProcessIcon(pkg)
            Column(Modifier.weight(1f)) {
                Text(
                    app?.label ?: pkg,
                    style = MaterialTheme.typography.titleMedium,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    group.process,
                    style = MaterialTheme.typography.mono,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        group.plugins.forEach { plugin ->
            HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.7f))
            Surface(
                onClick = { onOpenPlugin(plugin.id, group.process) },
                enabled = plugin.name != null,
                color = MaterialTheme.colorScheme.surface,
            ) {
                Row(
                    Modifier
                        .fillMaxWidth()
                        .heightIn(min = 52.dp)
                        .padding(start = 58.dp, end = 10.dp, top = 8.dp, bottom = 8.dp),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Row(
                        Modifier.weight(1f),
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(
                            plugin.name ?: plugin.id,
                            style = if (plugin.name != null) MaterialTheme.typography.bodyMedium else MaterialTheme.typography.mono,
                            modifier = Modifier.weight(1f, fill = false),
                            maxLines = 2,
                            overflow = TextOverflow.Ellipsis,
                        )
                        if (plugin.type != null) TypeTag(plugin.type)
                    }
                    if (plugin.name == null) {
                        Text(
                            stringResource(R.string.injecting_unknown_plugin),
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    } else {
                        Icon(
                            Icons.AutoMirrored.Rounded.KeyboardArrowRight,
                            contentDescription = null,
                            tint = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun ManualTargets(
    targets: List<String>,
    appsByPackage: Map<String, InstalledApp>,
    busy: Boolean,
    onRequestRemove: (String) -> Unit,
) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        SectionTitle(
            title = stringResource(R.string.injecting_manual_targets),
            modifier = Modifier.padding(start = 4.dp, top = 8.dp),
            subtitle = stringResource(R.string.injecting_manual_detail),
        )
        ManagerGroup {
            targets.forEachIndexed { index, target ->
                if (index > 0) HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.7f))
                val pkg = target.substringBefore(':')
                val app = appsByPackage[pkg]
                Row(
                    Modifier
                        .fillMaxWidth()
                        .heightIn(min = 56.dp)
                        .padding(start = 14.dp, end = 4.dp, top = 6.dp, bottom = 6.dp),
                    horizontalArrangement = Arrangement.spacedBy(12.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    ProcessIcon(pkg)
                    Column(Modifier.weight(1f)) {
                        if (app != null) {
                            Text(
                                app.label,
                                style = MaterialTheme.typography.bodyMedium,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                        }
                        Text(
                            target,
                            style = MaterialTheme.typography.mono,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            maxLines = 2,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                    IconButton(
                        onClick = { onRequestRemove(target) },
                        enabled = !busy,
                    ) {
                        Icon(
                            Icons.Rounded.Close,
                            contentDescription = stringResource(R.string.manual_target_remove_desc, target),
                            tint = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }
        }
    }
}
