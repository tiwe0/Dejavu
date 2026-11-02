package cafe.ivory.dejavu.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import cafe.ivory.dejavu.R
import cafe.ivory.dejavu.data.ConsoleUiState
import cafe.ivory.dejavu.data.DEFAULT_PLUGIN_DIRECTORY
import cafe.ivory.dejavu.data.DirectoryPluginState
import cafe.ivory.dejavu.data.DirectoryPluginUi
import cafe.ivory.dejavu.data.PackageEventsUi

@Composable
fun DirectoryPluginsSection(
    state: ConsoleUiState,
    onImport: () -> Unit,
    onRescan: () -> Unit,
    onStart: (String, String) -> Unit,
    onStop: (String, String) -> Unit,
    onEvents: (String, String) -> Unit,
) {
    val busy = state.pluginImporting || state.pluginDirectoryLoading || state.pluginActionId != null ||
        state.nativePluginActionPath != null
    Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
        SectionTitle(
            title = stringResource(R.string.package_directory_title),
            subtitle = stringResource(R.string.package_directory_description),
        )
        SelectionContainer {
            Text(
                text = DEFAULT_PLUGIN_DIRECTORY,
                style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(
                onClick = onImport,
                enabled = state.rootGranted && !busy,
                modifier = Modifier.weight(1f).heightIn(min = 48.dp),
            ) { Text(stringResource(R.string.package_import_folder)) }
            OutlinedButton(
                onClick = onRescan,
                enabled = state.rootGranted && !busy,
                modifier = Modifier.weight(1f).heightIn(min = 48.dp),
            ) { Text(stringResource(R.string.package_scan)) }
        }
        if (state.pluginImporting || state.pluginDirectoryLoading) {
            LinearProgressIndicator(Modifier.fillMaxWidth())
            Text(
                text = stringResource(
                    if (state.pluginImporting) R.string.package_importing else R.string.package_scanning,
                ),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        state.pluginDirectoryError?.let { error ->
            Text(
                text = error,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.error,
            )
        }
        if (state.directoryPlugins.isEmpty() && !state.pluginDirectoryLoading &&
            !state.pluginImporting && state.pluginDirectoryError == null
        ) {
            EmptyState(
                title = stringResource(R.string.package_empty_title),
                detail = stringResource(R.string.package_empty_detail),
            )
        }
        state.directoryPlugins.forEach { item ->
            DirectoryPluginCard(
                item = item,
                target = state.selectedTarget,
                busy = busy,
                working = state.pluginActionId == item.plugin.manifest.id,
                onCheck = onRescan,
                onStart = onStart,
                onStop = onStop,
                onEvents = onEvents,
            )
        }
    }
}

@Composable
private fun DirectoryPluginCard(
    item: DirectoryPluginUi,
    target: String?,
    busy: Boolean,
    working: Boolean,
    onCheck: () -> Unit,
    onStart: (String, String) -> Unit,
    onStop: (String, String) -> Unit,
    onEvents: (String, String) -> Unit,
) {
    val manifest = item.plugin.manifest
    val status = when (item.runState) {
        DirectoryPluginState.UNKNOWN -> R.string.package_state_unknown
        DirectoryPluginState.STOPPED -> R.string.package_state_stopped
        DirectoryPluginState.RUNNING -> R.string.package_state_running
        DirectoryPluginState.ERROR -> R.string.package_state_error
    }
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.medium,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        border = CardDefaults.outlinedCardBorder(),
    ) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Row(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.Top,
            ) {
                Text(
                    text = manifest.name,
                    modifier = Modifier.weight(1f),
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                )
                Text(
                    text = manifest.version,
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.primary,
                )
            }
            if (manifest.description.isNotBlank()) {
                Text(
                    text = manifest.description,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            StatusPill(
                label = stringResource(status),
                active = item.runState == DirectoryPluginState.RUNNING,
                activeColor = MaterialTheme.colorScheme.primary,
            )
            if (working) {
                LinearProgressIndicator(Modifier.fillMaxWidth())
                Text(
                    text = stringResource(R.string.package_working),
                    style = MaterialTheme.typography.bodySmall,
                )
            }
            item.error?.let { error ->
                Text(error, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
            }
            if (target == null) {
                Text(
                    text = stringResource(R.string.package_no_target),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                when (item.runState) {
                    DirectoryPluginState.UNKNOWN -> OutlinedButton(
                        onClick = onCheck,
                        enabled = target != null && !busy,
                        modifier = Modifier.weight(1f).heightIn(min = 48.dp),
                    ) { Text(stringResource(R.string.package_check_status)) }
                    DirectoryPluginState.STOPPED -> Button(
                        onClick = { if (target != null) onStart(target, manifest.id) },
                        enabled = target != null && !busy,
                        modifier = Modifier.weight(1f).heightIn(min = 48.dp),
                    ) { Text(stringResource(R.string.package_start)) }
                    DirectoryPluginState.RUNNING, DirectoryPluginState.ERROR -> OutlinedButton(
                        onClick = { if (target != null) onStop(target, manifest.id) },
                        enabled = target != null && !busy,
                        modifier = Modifier.weight(1f).heightIn(min = 48.dp),
                    ) {
                        Text(stringResource(
                            if (item.runState == DirectoryPluginState.ERROR) R.string.package_retry_stop
                            else R.string.package_stop,
                        ))
                    }
                }
                if (manifest.events != null) {
                    OutlinedButton(
                        onClick = { if (target != null) onEvents(target, manifest.id) },
                        enabled = target != null && !busy &&
                            item.runState == DirectoryPluginState.RUNNING && !item.session.isNullOrBlank(),
                        modifier = Modifier.weight(1f).heightIn(min = 48.dp),
                    ) { Text(stringResource(R.string.package_live_events)) }
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DirectoryPluginEventsSheet(state: PackageEventsUi, onDismiss: () -> Unit) {
    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 20.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Row(verticalAlignment = Alignment.Top) {
                Column(Modifier.weight(1f)) {
                    Text(state.packageName, style = MaterialTheme.typography.titleLarge)
                    Text(state.target, style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                TextButton(onClick = onDismiss, modifier = Modifier.heightIn(min = 48.dp)) {
                    Text(stringResource(R.string.package_close_events))
                }
            }
            Text(
                text = stringResource(R.string.package_events_memory_only),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            StatusPill(
                label = stringResource(
                    when {
                        state.error != null -> R.string.package_events_disconnected
                        state.connected -> R.string.package_events_listening
                        else -> R.string.package_events_connecting
                    },
                ),
                active = state.connected,
                activeColor = MaterialTheme.colorScheme.primary,
            )
            state.error?.let { error ->
                Text(error, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodyMedium)
            }
            LazyColumn(
                modifier = Modifier.fillMaxWidth().heightIn(max = 480.dp),
                contentPadding = PaddingValues(bottom = 32.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                if (state.events.isEmpty() && state.error == null) {
                    item {
                        EmptyState(
                            title = stringResource(R.string.package_events_empty_title),
                            detail = stringResource(R.string.package_events_empty_detail),
                        )
                    }
                }
                itemsIndexed(state.events) { _, event ->
                    Card(
                        modifier = Modifier.fillMaxWidth(),
                        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
                        border = CardDefaults.outlinedCardBorder(),
                    ) {
                        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                            Text(event.title, style = MaterialTheme.typography.titleSmall)
                            SelectionContainer { Text(event.content, style = MaterialTheme.typography.bodyMedium) }
                            if (event.meta.isNotBlank()) {
                                Text(event.meta, style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                        }
                    }
                }
            }
            Spacer(Modifier.height(8.dp))
        }
    }
}
