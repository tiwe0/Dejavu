package cafe.ivory.dejavu.ui

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.BorderStroke
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
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.MoreVert
import androidx.compose.material.icons.rounded.PlayArrow
import androidx.compose.material.icons.rounded.Settings
import androidx.compose.material.icons.rounded.Stop
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.ModalBottomSheetProperties
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.SecureFlagPolicy
import cafe.ivory.dejavu.R
import cafe.ivory.dejavu.data.DirectoryPluginState
import cafe.ivory.dejavu.data.DirectoryPluginUi
import cafe.ivory.dejavu.data.LivePluginEvent
import cafe.ivory.dejavu.data.PackageEventsUi
import cafe.ivory.dejavu.data.PluginPageAction
import cafe.ivory.dejavu.data.PluginPageInfoRow
import cafe.ivory.dejavu.data.PluginPagePreset
import cafe.ivory.dejavu.data.validPluginListenPort
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

@Composable
internal fun PluginPresetLayout(
    preset: PluginPagePreset,
    plugin: DirectoryPluginUi,
    target: String?,
    busy: Boolean,
    inScope: Boolean,
    working: Boolean,
    runtimeError: String?,
    targetOffline: Boolean,
    events: PackageEventsUi?,
    usesSocket: Boolean,
    preferredPort: Int,
    onCheck: () -> Unit,
    onStart: () -> Unit,
    onStop: () -> Unit,
    onCall: (String) -> Unit,
    onReconnect: () -> Unit,
    onApplyPort: (Int) -> Unit,
    targetInHeader: Boolean = false,
) {
    val monitor = preset as? PluginPagePreset.Monitor
    val running = !targetOffline && plugin.runState == DirectoryPluginState.RUNNING && plugin.session != null
    val canRunAction = inScope && target != null && running && !busy && (!usesSocket || events?.connected == true)
    var showSettings by remember(plugin.plugin.manifest.id, target, usesSocket) { mutableStateOf(false) }

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(start = 16.dp, top = 8.dp, end = 16.dp, bottom = 24.dp),
    ) {
        item {
            RuntimePanel(plugin, target, busy, inScope, working, runtimeError, targetOffline, targetInHeader, onCheck, onStart, onStop)
        }
        if (monitor != null) {
            if (usesSocket) {
                item {
                    SocketSettingsRow(
                        address = listenerAddress(events?.listenPort.takeIf { events?.error == null }, preferredPort),
                        enabled = inScope && target != null && !busy,
                        onSettings = { showSettings = true },
                        modifier = Modifier.padding(top = 12.dp),
                    )
                }
            }
            val liveEvents = events?.events.orEmpty().takeLast(monitor.eventLimit).asReversed()
            item {
                MonitorHeading(
                    running = running,
                    events = events,
                    actions = monitor.actions,
                    canRunAction = canRunAction,
                    onCall = onCall,
                    onReconnect = onReconnect,
                    canReconnect = inScope && target != null && !busy,
                    empty = liveEvents.isEmpty(),
                    modifier = Modifier.padding(top = 12.dp, bottom = 8.dp),
                )
            }
            itemsIndexed(liveEvents) { index, event ->
                MonitorEventRow(event, first = index == 0, last = index == liveEvents.lastIndex)
            }
        } else if (preset is PluginPagePreset.Controls) {
            if (preset.rows.isNotEmpty()) {
                item { InfoRows(preset.rows, Modifier.padding(top = 12.dp)) }
            }
            if (preset.actions.isNotEmpty()) {
                item {
                    ControlActions(
                        actions = preset.actions,
                        enabled = canRunAction,
                        onCall = onCall,
                        modifier = Modifier.padding(top = 12.dp),
                    )
                }
            }
        }
    }
    if (showSettings) {
        PortSheet(
            activePort = events?.listenPort.takeIf { events?.error == null },
            preferredPort = preferredPort,
            running = running,
            enabled = inScope && target != null && !busy,
            onDismiss = { showSettings = false },
            onApply = { port ->
                showSettings = false
                onApplyPort(port)
            },
        )
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun RuntimePanel(
    plugin: DirectoryPluginUi,
    target: String?,
    busy: Boolean,
    inScope: Boolean,
    working: Boolean,
    runtimeError: String?,
    targetOffline: Boolean,
    targetInHeader: Boolean,
    onCheck: () -> Unit,
    onStart: () -> Unit,
    onStop: () -> Unit,
) {
    val state = plugin.runState
    val needsCheck = target != null && (targetOffline || state == DirectoryPluginState.UNKNOWN)
    val canStop = state == DirectoryPluginState.RUNNING || state == DirectoryPluginState.ERROR
    val canToggle = target != null && !busy && !targetOffline &&
        (canStop || (inScope && state == DirectoryPluginState.STOPPED))

    PresetCard {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            FlowRow(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                Column {
                    Text(
                        stringResource(if (targetInHeader) R.string.plugin_detail_runtime else R.string.target_process),
                        style = if (targetInHeader) MaterialTheme.typography.titleSmall else MaterialTheme.typography.bodySmall,
                        color = if (targetInHeader) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    if (!targetInHeader) {
                        Text(
                            target ?: stringResource(R.string.package_page_no_target),
                            style = MaterialTheme.typography.bodyLarge,
                            fontWeight = FontWeight.SemiBold,
                        )
                    }
                }
                PresetBadge(
                    label = stringResource(if (targetOffline) R.string.package_page_target_offline else state.labelRes),
                    active = !targetOffline && state == DirectoryPluginState.RUNNING,
                )
            }
            runtimeError?.let { ErrorBanner(it) }
            plugin.error?.takeIf { it != runtimeError }?.let { ErrorBanner(it) }
            if (working) {
                LinearProgressIndicator(Modifier.fillMaxWidth())
                Text(stringResource(R.string.package_working), style = MaterialTheme.typography.bodySmall)
            }
            FlowRow(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.End),
                verticalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                if (needsCheck) {
                    TextButton(onClick = onCheck, enabled = !busy, modifier = Modifier.heightIn(min = 48.dp)) {
                        Text(stringResource(R.string.package_check_status))
                    }
                } else {
                    Text(
                        stringResource(R.string.package_page_version, plugin.plugin.manifest.version),
                        modifier = Modifier.weight(1f).align(Alignment.CenterVertically),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                if (canStop && !targetOffline) {
                    OutlinedButton(
                        onClick = onStop,
                        enabled = canToggle,
                        modifier = Modifier.heightIn(min = 48.dp),
                        shape = MaterialTheme.shapes.medium,
                    ) {
                        Icon(Icons.Rounded.Stop, contentDescription = null, modifier = Modifier.size(18.dp))
                        Text(stringResource(if (state == DirectoryPluginState.ERROR) R.string.package_retry_stop
                            else R.string.package_stop), Modifier.padding(start = 6.dp))
                    }
                } else {
                    Button(
                        onClick = onStart,
                        enabled = canToggle,
                        modifier = Modifier.heightIn(min = 48.dp),
                        shape = MaterialTheme.shapes.medium,
                    ) {
                        Icon(Icons.Rounded.PlayArrow, contentDescription = null, modifier = Modifier.size(18.dp))
                        Text(stringResource(R.string.package_start), Modifier.padding(start = 6.dp))
                    }
                }
            }
        }
    }
}

@Composable
private fun SocketSettingsRow(address: String, enabled: Boolean, onSettings: () -> Unit, modifier: Modifier) {
    PresetCard(modifier) {
        Row(
            Modifier.fillMaxWidth().padding(start = 12.dp, end = 6.dp, top = 4.dp, bottom = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(Modifier.weight(1f)) {
                Text(stringResource(R.string.package_page_address), style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                Text(address, style = MaterialTheme.typography.bodyMedium, fontWeight = FontWeight.SemiBold)
            }
            TextButton(onClick = onSettings, enabled = enabled, modifier = Modifier.heightIn(min = 48.dp)) {
                Icon(Icons.Rounded.Settings, contentDescription = null, modifier = Modifier.size(18.dp))
                Text(stringResource(R.string.package_page_settings), Modifier.padding(start = 4.dp))
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)
@Composable
private fun PortSheet(
    activePort: Int?,
    preferredPort: Int,
    running: Boolean,
    enabled: Boolean,
    onDismiss: () -> Unit,
    onApply: (Int) -> Unit,
) {
    var portDraft by remember(preferredPort) { mutableStateOf(preferredPort.toString()) }
    val validPort = portDraft.toIntOrNull()?.takeIf(::validPluginListenPort)
    ModalBottomSheet(
        onDismissRequest = onDismiss,
        sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true),
        properties = ModalBottomSheetProperties(securePolicy = SecureFlagPolicy.SecureOn),
    ) {
        Column(
            Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(start = 16.dp, end = 16.dp, bottom = 16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            SectionTitle(
                title = stringResource(R.string.package_page_socket_settings),
                subtitle = activePort?.let { stringResource(R.string.package_page_listen_address, it) }
                    ?: listenerAddress(null, preferredPort),
            )
            OutlinedTextField(
                value = portDraft,
                onValueChange = { if (it.length <= 5 && it.all { c -> c in '0'..'9' }) portDraft = it },
                label = { Text(stringResource(R.string.package_page_port_label)) },
                supportingText = {
                    Text(stringResource(if (validPort == null) R.string.package_page_invalid_port
                        else R.string.package_page_port_hint))
                },
                isError = validPort == null,
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                modifier = Modifier.fillMaxWidth(),
            )
            if (running) {
                Text(stringResource(R.string.package_page_reconnect_notice),
                    style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            FlowRow(
                Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.End),
            ) {
                TextButton(onClick = onDismiss, modifier = Modifier.heightIn(min = 48.dp)) {
                    Text(stringResource(R.string.cancel))
                }
                Button(
                    onClick = { validPort?.let(onApply) },
                    enabled = enabled && validPort != null,
                    modifier = Modifier.heightIn(min = 48.dp),
                ) {
                    Text(stringResource(R.string.package_page_apply_port))
                }
            }
        }
    }
}

@Composable
private fun listenerAddress(activePort: Int?, preferredPort: Int): String = when {
    activePort != null -> stringResource(R.string.package_page_socket_address, activePort)
    preferredPort != 0 -> stringResource(R.string.package_page_socket_pending_port, preferredPort)
    else -> stringResource(R.string.package_page_socket_auto)
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun MonitorHeading(
    running: Boolean,
    events: PackageEventsUi?,
    actions: List<PluginPageAction>,
    canRunAction: Boolean,
    onCall: (String) -> Unit,
    onReconnect: () -> Unit,
    canReconnect: Boolean,
    empty: Boolean,
    modifier: Modifier,
) {
    val disconnected = running && events?.error != null
    val connected = running && events?.connected == true
    Column(modifier, verticalArrangement = Arrangement.spacedBy(4.dp)) {
        FlowRow(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            Text(
                stringResource(R.string.package_live_events),
                modifier = Modifier.align(Alignment.CenterVertically),
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold,
            )
            PresetBadge(
                label = stringResource(when {
                    disconnected -> R.string.package_events_disconnected
                    connected -> R.string.package_page_connected
                    running -> R.string.package_events_connecting
                    else -> R.string.package_page_not_listening
                }),
                active = connected,
                modifier = Modifier.align(Alignment.CenterVertically),
            )
            if (actions.isNotEmpty() && running && !disconnected) {
                Row(Modifier.weight(1f), horizontalArrangement = Arrangement.End) {
                    when (actions.size) {
                        1 -> TextButton(
                            onClick = { onCall(actions.first().id) },
                            enabled = canRunAction,
                            modifier = Modifier.heightIn(min = 48.dp),
                        ) { Text(actions.first().label.localized(), style = MaterialTheme.typography.labelMedium) }
                        else -> MorePluginActions(actions, canRunAction, onCall)
                    }
                }
            }
        }
        Text(
            stringResource(R.string.package_page_event_privacy),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        if (disconnected) {
            ErrorBanner(checkNotNull(events?.error))
            TextButton(onClick = onReconnect, enabled = canReconnect, modifier = Modifier.heightIn(min = 48.dp)) {
                Text(stringResource(R.string.package_page_reconnect))
            }
        } else if (empty && (connected || !running)) {
            PresetCard(Modifier.padding(top = 4.dp)) {
                Text(
                    stringResource(if (connected) R.string.package_events_empty_title
                        else R.string.package_page_events_start_first),
                    modifier = Modifier.padding(12.dp),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

@Composable
private fun MonitorEventRow(event: LivePluginEvent, first: Boolean, last: Boolean) {
    val corners = RoundedCornerShape(
        topStart = if (first) 14.dp else 0.dp,
        topEnd = if (first) 14.dp else 0.dp,
        bottomStart = if (last) 14.dp else 0.dp,
        bottomEnd = if (last) 14.dp else 0.dp,
    )
    val received = event.receivedAtMillis
    var fresh by remember(event) { mutableStateOf(first && received != null && System.currentTimeMillis() - received in 0L..500L) }
    LaunchedEffect(event) { fresh = false }
    val color by animateColorAsState(
        if (fresh) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surface,
        animationSpec = tween(170),
        label = "recent plugin event",
    )
    Surface(modifier = Modifier.fillMaxWidth(), shape = corners, color = color) {
        Column {
            if (!first) HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
            Column(Modifier.padding(horizontal = 12.dp, vertical = 9.dp), verticalArrangement = Arrangement.spacedBy(3.dp)) {
                Text(event.title, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold)
                Text(event.content, style = MaterialTheme.typography.bodyMedium)
                if (event.meta.isNotBlank() || received != null) {
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text(event.meta, Modifier.weight(1f), style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                        if (received != null) {
                            val locale = LocalConfiguration.current.locales[0] ?: Locale.getDefault()
                            val time = remember(received, locale) { SimpleDateFormat("HH:mm:ss", locale).format(Date(received)) }
                            Text(time, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun InfoRows(rows: List<PluginPageInfoRow>, modifier: Modifier) {
    PresetCard(modifier) {
        Column {
            rows.forEachIndexed { index, row ->
                if (index > 0) HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                Column(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 10.dp)) {
                    Text(row.label.localized(), style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Text(row.value.localized(), style = MaterialTheme.typography.bodyMedium)
                }
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun ControlActions(
    actions: List<PluginPageAction>,
    enabled: Boolean,
    onCall: (String) -> Unit,
    modifier: Modifier,
) {
    val primary = actions.first()
    Column(modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Button(
            onClick = { onCall(primary.id) },
            enabled = enabled,
            modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
        ) { Text(primary.label.localized()) }
        if (actions.size > 1) {
            FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                for (action in actions.drop(1)) {
                    TextButton(
                        onClick = { onCall(action.id) },
                        enabled = enabled,
                        modifier = Modifier.heightIn(min = 48.dp),
                    ) { Text(action.label.localized()) }
                }
            }
        }
    }
}

@Composable
private fun MorePluginActions(actions: List<PluginPageAction>, enabled: Boolean, onCall: (String) -> Unit) {
    var expanded by remember { mutableStateOf(false) }
    Box {
        IconButton(onClick = { expanded = true }, enabled = enabled) {
            Icon(Icons.Rounded.MoreVert, contentDescription = stringResource(R.string.package_page_more_actions))
        }
        DropdownMenu(
            expanded = expanded && enabled,
            onDismissRequest = { expanded = false },
        ) {
            for (action in actions) {
                DropdownMenuItem(
                    text = { Text(action.label.localized()) },
                    onClick = {
                        expanded = false
                        onCall(action.id)
                    },
                )
            }
        }
    }
}

@Composable
private fun PresetBadge(label: String, active: Boolean, modifier: Modifier = Modifier) {
    Surface(
        modifier = modifier,
        shape = CircleShape,
        color = if (active) MaterialTheme.colorScheme.secondaryContainer else MaterialTheme.colorScheme.surfaceVariant,
        contentColor = if (active) MaterialTheme.colorScheme.onSecondaryContainer else MaterialTheme.colorScheme.onSurfaceVariant,
    ) {
        Row(
            Modifier.padding(horizontal = 9.dp, vertical = 4.dp),
            horizontalArrangement = Arrangement.spacedBy(5.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Box(Modifier.size(6.dp).background(if (active) MaterialTheme.colorScheme.onSecondaryContainer
                else MaterialTheme.colorScheme.outline, CircleShape))
            Text(label, style = MaterialTheme.typography.labelMedium)
        }
    }
}

@Composable
private fun PresetCard(modifier: Modifier = Modifier, content: @Composable () -> Unit) {
    Surface(
        modifier = modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.large,
        color = MaterialTheme.colorScheme.surface,
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant),
        content = content,
    )
}
