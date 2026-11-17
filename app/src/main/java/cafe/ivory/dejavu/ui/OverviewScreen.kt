package cafe.ivory.dejavu.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.layout.Box
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.KeyboardArrowRight
import androidx.compose.material.icons.automirrored.rounded.OpenInNew
import androidx.compose.material.icons.rounded.AccountTree
import androidx.compose.material.icons.rounded.Code
import androidx.compose.material.icons.rounded.Gavel
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import cafe.ivory.dejavu.R
import cafe.ivory.dejavu.data.ConsoleUiState
import cafe.ivory.dejavu.data.InstalledApp
import cafe.ivory.dejavu.data.ModuleUiState
import cafe.ivory.dejavu.ui.theme.mono

private const val PROJECT_URL = "https://github.com/tiwe0/Dejavu"

private data class ScopedPlugin(val id: String, val name: String?, val type: String?)
private data class ProcessGroup(val process: String, val plugins: List<ScopedPlugin>)

@Composable
internal fun OverviewScreen(
    state: ConsoleUiState,
    moduleState: ModuleUiState,
    onManageModule: () -> Unit,
    onManagePlugins: () -> Unit,
    onRefresh: () -> Unit,
    onOpenPlugin: (id: String, process: String) -> Unit,
) {
    val groups = remember(state.pluginScopes, state.directoryPlugins, state.nativeLibrary) { processGroups(state) }
    val scoped = groups.map { it.process }.toSet()
    val manual = state.targets.filter { it !in scoped }.distinct()
    // Reveal once per session, and only after the library scan has produced the real groups.
    val reveal = remember { RevealMemory.firstTime("overview") }
    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(start = 16.dp, top = 4.dp, end = 16.dp, bottom = 24.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item(key = "chain") { Reveal(0, reveal) { OverviewChain(state, moduleState, onManageModule, onRefresh) } }
        item(key = "injecting-title") {
            Reveal(1, reveal) {
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
                    Text(
                        stringResource(R.string.injecting_summary, groups.size, groups.flatMap { it.plugins }.distinctBy { it.id }.size),
                        style = MaterialTheme.typography.labelMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
            }
        }
        if (groups.isEmpty() && !state.loading) {
            item(key = "injecting-empty") {
                Box(Modifier.animateItem()) { Reveal(2, reveal) {
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
                            textAlign = androidx.compose.ui.text.style.TextAlign.Center,
                        )
                        FilledTonalButton(onClick = onManagePlugins, modifier = Modifier.heightIn(min = 48.dp)) {
                            Text(stringResource(R.string.injecting_open_plugins))
                        }
                    }
                }
                } }
            }
        }
        itemsIndexed(groups, key = { _, it -> "process:${it.process}" }) { index, group ->
            Box(Modifier.animateItem()) {
                Reveal(2 + index, reveal) { ProcessGroupCard(group, state.installedApps, onOpenPlugin) }
            }
        }
        if (manual.isNotEmpty()) {
            item(key = "manual") {
                Box(Modifier.animateItem()) { Reveal(2 + groups.size, reveal) { ManualTargets(manual, state.installedApps) } }
            }
        }
    }
}

private fun processGroups(state: ConsoleUiState): List<ProcessGroup> {
    val lua = state.directoryPlugins.associateBy { it.plugin.manifest.id }
    val native = state.nativeLibrary.mapNotNull { item -> item.artifact.id?.let { it to item } }.toMap()
    val byProcess = linkedMapOf<String, MutableList<ScopedPlugin>>()
    state.pluginScopes.toSortedMap().forEach { (id, processes) ->
        val plugin = lua[id]?.let { ScopedPlugin(id, it.plugin.manifest.name, "LUA") }
            ?: native[id]?.let { ScopedPlugin(id, it.artifact.name, if (it.artifact.jit) "JIT" else "NATIVE") }
            ?: ScopedPlugin(id, null, null)
        processes.distinct().forEach { byProcess.getOrPut(it) { mutableListOf() }.add(plugin) }
    }
    return byProcess.entries.sortedBy { it.key }.map { ProcessGroup(it.key, it.value) }
}

@Composable
private fun OverviewChain(
    state: ConsoleUiState,
    moduleState: ModuleUiState,
    onManageModule: () -> Unit,
    onRefresh: () -> Unit,
) {
    val snapshot = moduleState.snapshot
    val moduleOk = state.moduleInstalled && snapshot?.disabled != true && snapshot?.removalPending != true
    val moduleDetail = when {
        moduleState.working -> stringResource(R.string.module_install_progress)
        snapshot?.removalPending == true -> stringResource(R.string.module_install_removing)
        snapshot?.rebootPending == true -> stringResource(R.string.module_install_pending)
        snapshot?.disabled == true -> stringResource(R.string.module_install_disabled)
        state.moduleInstalled && state.moduleVersion.isNotBlank() -> state.moduleVersion
        state.moduleInstalled -> stringResource(R.string.overview_module_detected)
        else -> stringResource(R.string.chain_missing)
    }
    val helperDetail = if (state.helperRunning) {
        state.helperPort.takeIf { it.isNotBlank() }?.let { ":$it" } ?: stringResource(R.string.overview_running)
    } else stringResource(R.string.overview_offline)
    InjectionChain(
        listOf(
            ChainLink(
                label = stringResource(R.string.chain_module),
                detail = moduleDetail,
                ok = moduleOk,
                actionLabel = stringResource(if (state.moduleInstalled) R.string.chain_open_module else R.string.chain_install_module),
                onAction = onManageModule,
                onClick = onManageModule,
            ),
            ChainLink(stringResource(R.string.chain_root), stringResource(R.string.overview_root_granted), ok = true),
            ChainLink(
                label = stringResource(R.string.chain_helper),
                detail = helperDetail,
                ok = state.helperRunning,
                actionLabel = stringResource(R.string.chain_recheck_helper),
                onAction = onRefresh,
            ),
        ),
        refreshing = state.loading || moduleState.loading,
    )
}

@Composable
private fun ProcessGroupCard(
    group: ProcessGroup,
    apps: List<InstalledApp>,
    onOpenPlugin: (String, String) -> Unit,
) {
    val app = apps.firstOrNull { it.packageName == group.process.substringBefore(':') }
    ManagerGroup {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 12.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            ProcessIcon(app)
            Column(Modifier.weight(1f)) {
                Text(
                    app?.label ?: group.process.substringBefore(':'),
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
                    Modifier.fillMaxWidth().heightIn(min = 52.dp).padding(start = 58.dp, end = 10.dp, top = 8.dp, bottom = 8.dp),
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
private fun ManualTargets(targets: List<String>, apps: List<InstalledApp>) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        SectionTitle(
            stringResource(R.string.injecting_manual_targets),
            stringResource(R.string.injecting_manual_detail),
            modifier = Modifier.padding(start = 4.dp, top = 8.dp),
        )
        ManagerGroup {
            targets.forEachIndexed { index, target ->
                if (index > 0) HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.7f))
                Row(
                    Modifier.fillMaxWidth().heightIn(min = 52.dp).padding(horizontal = 14.dp, vertical = 8.dp),
                    horizontalArrangement = Arrangement.spacedBy(12.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    val app = apps.firstOrNull { it.packageName == target.substringBefore(':') }
                    ProcessIcon(app)
                    Column(Modifier.weight(1f)) {
                        if (app != null) Text(app.label, style = MaterialTheme.typography.bodyMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
                        Text(target, style = MaterialTheme.typography.mono, color = MaterialTheme.colorScheme.onSurfaceVariant,
                            maxLines = 2, overflow = TextOverflow.Ellipsis)
                    }
                }
            }
        }
    }
}

@Composable
internal fun ProjectReferences(onLinkUnavailable: (String) -> Unit) {
    val uriHandler = LocalUriHandler.current
    val unavailable = stringResource(R.string.overview_link_unavailable)
    fun open(url: String) {
        try {
            uriHandler.openUri(url)
        } catch (_: IllegalArgumentException) {
            onLinkUnavailable(unavailable)
        }
    }
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        SectionTitle(stringResource(R.string.configuration_about))
        ManagerGroup {
            ManagerLinkRow(
                Icons.Rounded.Code, stringResource(R.string.overview_source), "tiwe0/Dejavu",
                Icons.AutoMirrored.Rounded.OpenInNew,
            ) { open(PROJECT_URL) }
            HorizontalDivider()
            ManagerLinkRow(
                Icons.Rounded.AccountTree, stringResource(R.string.overview_references),
                stringResource(R.string.overview_reference_components),
                Icons.AutoMirrored.Rounded.OpenInNew,
            ) { open("$PROJECT_URL/blob/main/THIRD_PARTY_NOTICES.md") }
            HorizontalDivider()
            ManagerLinkRow(
                Icons.Rounded.Gavel, stringResource(R.string.overview_license),
                stringResource(R.string.overview_copyright),
                Icons.AutoMirrored.Rounded.OpenInNew,
            ) { open("$PROJECT_URL/blob/main/LICENSE") }
        }
    }
}

@Composable
internal fun ManagerGroup(content: @Composable () -> Unit) {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.large,
        color = MaterialTheme.colorScheme.surface,
        border = CardDefaults.outlinedCardBorder(),
    ) {
        Column { content() }
    }
}

@Composable
internal fun ManagerLinkRow(
    icon: ImageVector,
    label: String,
    detail: String,
    trailingIcon: ImageVector,
    onClick: () -> Unit,
) {
    Surface(onClick = onClick, modifier = Modifier.fillMaxWidth(), color = MaterialTheme.colorScheme.surface) {
        Row(
            modifier = Modifier.heightIn(min = 60.dp).padding(horizontal = 14.dp, vertical = 10.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(icon, contentDescription = null, tint = MaterialTheme.colorScheme.primary)
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(label, style = MaterialTheme.typography.titleSmall)
                Text(detail, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Icon(
                trailingIcon,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.size(18.dp),
            )
        }
    }
}
