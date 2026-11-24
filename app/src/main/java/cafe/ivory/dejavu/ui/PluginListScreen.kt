package cafe.ivory.dejavu.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
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
import androidx.compose.material.icons.rounded.Add
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExtendedFloatingActionButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.disabled
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import cafe.ivory.dejavu.R
import cafe.ivory.dejavu.data.EnvironmentUiState
import cafe.ivory.dejavu.data.PluginsUiState
import cafe.ivory.dejavu.data.operationInProgress
import kotlinx.coroutines.delay

@Composable
internal fun PluginListScreen(
    envState: EnvironmentUiState,
    pluginsState: PluginsUiState,
    onOpenPlugin: (String) -> Unit,
    onImportZip: () -> Unit,
    preview: Boolean = false,
) {
    val items = remember(pluginsState.directoryPlugins, pluginsState.nativeLibrary) {
        pluginsState.allLibraryItems
    }
    val busy = pluginsState.operationInProgress(envState.scopeSavingId)
    val importing = pluginsState.pluginImporting
    val scanning = pluginsState.pluginDirectoryLoading
    val canAdd = envState.rootGranted && !busy && !scanning
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
    val pinnedEdge by remember(listState) {
        derivedStateOf { !listState.canScrollBackward || !listState.canScrollForward }
    }
    val revealMemory = LocalRevealMemory.current
    val readyForReveal = !preview && items.isNotEmpty()
    var revealActive by remember { mutableStateOf(false) }
    LaunchedEffect(readyForReveal) {
        if (readyForReveal && revealMemory.consumeFirstTime("plugins")) {
            revealActive = true
            delay(550)
            revealActive = false
        }
    }
    LaunchedEffect(listState.isScrollInProgress) {
        if (listState.isScrollInProgress && revealActive) revealActive = false
    }
    val fabPress = remember { MutableInteractionSource() }

    Box(Modifier.fillMaxSize()) {
        LazyColumn(
            state = listState,
            modifier = Modifier.fillMaxSize(),
            contentPadding = PaddingValues(top = 4.dp, bottom = 96.dp),
            verticalArrangement = Arrangement.spacedBy(0.dp),
        ) {
            if (importing || scanning) {
                item(key = "progress") {
                    Column(
                        Modifier.padding(horizontal = 16.dp, vertical = 6.dp),
                        verticalArrangement = Arrangement.spacedBy(6.dp),
                    ) {
                        LinearProgressIndicator(Modifier.fillMaxWidth())
                        Text(
                            stringResource(if (importing) R.string.package_zip_importing else R.string.package_scanning),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }
            pluginsState.pluginDirectoryError?.let { error ->
                item(key = "scan-error") {
                    ErrorBanner(error, Modifier.padding(horizontal = 16.dp, vertical = 6.dp))
                }
            }
            if (items.isEmpty() && !importing && !scanning && pluginsState.pluginDirectoryError == null) {
                item(key = "empty") {
                    EmptyState(
                        title = stringResource(R.string.package_zip_empty_title),
                        detail = stringResource(R.string.package_zip_empty_detail),
                    )
                }
            }
            itemsIndexed(items, key = { _, item -> "${item.kindTag}:${item.id}" }) { index, item ->
                Box(Modifier.animateItem()) {
                    Reveal(index, revealActive) {
                        PluginListRow(
                            name = item.name,
                            tag = if (item.version.isBlank()) item.kindTag else "${item.kindTag} · ${item.version}",
                            scopeCount = envState.pluginScopes[item.id].orEmpty().size,
                            enabled = !busy && !scanning,
                            onOpen = { onOpenPlugin(item.id) },
                        )
                    }
                }
            }
        }
        AnimatedVisibility(
            visible = showAdd || importing || pinnedEdge,
            modifier = Modifier.align(Alignment.BottomEnd).padding(16.dp),
            enter = fadeIn() + slideInVertically { it },
            exit = fadeOut() + slideOutVertically { it },
        ) {
            ExtendedFloatingActionButton(
                onClick = { if (canAdd) onImportZip() },
                interactionSource = fabPress,
                modifier = Modifier
                    .pressScale(fabPress, 0.95f)
                    .semantics { if (!canAdd) disabled() },
                containerColor = if (canAdd || importing) MaterialTheme.colorScheme.primary
                else MaterialTheme.colorScheme.surfaceVariant,
                contentColor = if (canAdd || importing) MaterialTheme.colorScheme.onPrimary
                else MaterialTheme.colorScheme.onSurfaceVariant,
                icon = {
                    if (importing) {
                        CircularProgressIndicator(
                            Modifier.size(20.dp),
                            strokeWidth = 2.dp,
                            color = MaterialTheme.colorScheme.onPrimary,
                        )
                    } else {
                        Icon(Icons.Rounded.Add, contentDescription = null)
                    }
                },
                text = {
                    Text(stringResource(if (importing) R.string.package_zip_importing else R.string.plugin_import_zip))
                },
            )
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun PluginListRow(
    name: String,
    tag: String,
    scopeCount: Int,
    enabled: Boolean,
    onOpen: () -> Unit,
) {
    Column {
        Surface(onClick = onOpen, enabled = enabled, color = MaterialTheme.colorScheme.background) {
            Row(
                Modifier
                    .fillMaxWidth()
                    .heightIn(min = 72.dp)
                    .padding(horizontal = 16.dp, vertical = 12.dp),
                horizontalArrangement = Arrangement.spacedBy(14.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                PluginMonogram(name)
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text(
                        name,
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.Medium,
                        maxLines = 2,
                        overflow = TextOverflow.Ellipsis,
                    )
                    FlowRow(
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        verticalArrangement = Arrangement.spacedBy(4.dp),
                        itemVerticalAlignment = Alignment.CenterVertically,
                    ) {
                        TypeTag(tag)
                        Text(
                            if (scopeCount > 0) pluralStringResource(R.plurals.plugin_row_scope, scopeCount, scopeCount)
                            else stringResource(R.string.plugin_row_no_scope),
                            style = MaterialTheme.typography.bodySmall,
                            color = if (scopeCount > 0) MaterialTheme.colorScheme.onSurface
                            else MaterialTheme.colorScheme.outline,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                }
                Icon(
                    Icons.AutoMirrored.Rounded.KeyboardArrowRight,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        HorizontalDivider(Modifier.padding(start = 70.dp), color = MaterialTheme.colorScheme.outlineVariant)
    }
}
