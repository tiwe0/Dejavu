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
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.ModalBottomSheetProperties
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.SecureFlagPolicy
import cafe.ivory.dejavu.R
import cafe.ivory.dejavu.data.DirectoryPluginState
import cafe.ivory.dejavu.data.LivePluginEvent
import cafe.ivory.dejavu.data.PackageEventsUi
import cafe.ivory.dejavu.ui.theme.mono

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DirectoryPluginEventsSheet(state: PackageEventsUi, onDismiss: () -> Unit) {
    ModalBottomSheet(
        onDismissRequest = onDismiss,
        properties = ModalBottomSheetProperties(securePolicy = SecureFlagPolicy.SecureOn),
    ) {
        Column(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 20.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Row(verticalAlignment = Alignment.Top) {
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    Text(state.packageName, style = MaterialTheme.typography.titleLarge)
                    Text(
                        state.target,
                        style = MaterialTheme.typography.mono,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
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
            LiveStatus(
                label = stringResource(
                    when {
                        state.error != null -> R.string.package_events_disconnected
                        state.connected -> R.string.package_events_listening
                        else -> R.string.package_events_connecting
                    },
                ),
                live = state.connected,
                error = state.error != null,
            )
            state.error?.let { error -> ErrorBanner(error) }
            LazyColumn(
                modifier = Modifier.fillMaxWidth().weight(1f, fill = false),
                contentPadding = PaddingValues(bottom = 32.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                if (state.events.isEmpty() && state.error == null) {
                    item(key = "empty") {
                        EmptyState(
                            title = stringResource(R.string.package_events_empty_title),
                            detail = stringResource(R.string.package_events_empty_detail),
                        )
                    }
                }
                items(state.events, key = { it.id }) { event ->
                    PluginEventCard(event, selectable = true)
                }
            }
            Spacer(Modifier.height(8.dp))
        }
    }
}

internal val DirectoryPluginState.labelRes: Int
    get() = when (this) {
        DirectoryPluginState.UNKNOWN -> R.string.package_state_unknown
        DirectoryPluginState.STOPPED -> R.string.package_state_stopped
        DirectoryPluginState.RUNNING -> R.string.package_state_running
        DirectoryPluginState.ERROR -> R.string.package_state_error
    }

@Composable
internal fun PluginEventCard(event: LivePluginEvent, selectable: Boolean = false) {
    ManagerGroup {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Text(event.title, style = MaterialTheme.typography.titleSmall)
            if (selectable) {
                SelectionContainer { Text(event.content, style = MaterialTheme.typography.bodyMedium) }
            } else {
                Text(event.content, style = MaterialTheme.typography.bodyMedium)
            }
            if (event.meta.isNotBlank()) {
                Text(
                    event.meta,
                    style = MaterialTheme.typography.mono,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}
