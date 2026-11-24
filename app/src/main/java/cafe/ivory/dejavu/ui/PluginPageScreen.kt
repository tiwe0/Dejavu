package cafe.ivory.dejavu.ui

import android.view.WindowManager
import androidx.activity.compose.LocalActivity
import androidx.compose.foundation.BorderStroke
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.ArrowBack
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import cafe.ivory.dejavu.R
import cafe.ivory.dejavu.data.DirectoryPluginState
import cafe.ivory.dejavu.data.DirectoryPluginUi
import cafe.ivory.dejavu.data.PackageEventsUi
import cafe.ivory.dejavu.data.PluginPage
import cafe.ivory.dejavu.data.PluginPageBlock
import cafe.ivory.dejavu.data.PluginPageText
import cafe.ivory.dejavu.data.validPluginListenPort

@Composable
internal fun PluginPageScreen(
    page: PluginPage,
    plugin: DirectoryPluginUi,
    target: String?,
    busy: Boolean,
    working: Boolean,
    runtimeError: String?,
    events: PackageEventsUi?,
    onBack: () -> Unit,
    onCheck: () -> Unit,
    onStart: (String, String) -> Unit,
    onStop: (String, String) -> Unit,
    onCall: (String, String, String) -> Unit,
    onListen: (String, String, Int?) -> Unit,
    onCloseEvents: () -> Unit,
    preferredPort: Int = 0,
    onSetPreferredPort: (Int) -> Unit = {},
    targetOffline: Boolean = false,
    inScope: Boolean = true,
    hoistHeader: Boolean = false,
    preview: Boolean = false,
) {
    val id = plugin.plugin.manifest.id
    val preset = page.preset
    val hasEvents = page.eventLimit != null
    val usesSocket = hasEvents && plugin.plugin.manifest.events?.usesSocket == true
    val session = plugin.session.takeIf {
        plugin.runState == DirectoryPluginState.RUNNING && (preset == null || !targetOffline)
    }
    var portDraft by rememberSaveable(id, target) {
        mutableStateOf(if (preferredPort > 0) preferredPort.toString() else "0")
    }
    var appliedPort by rememberSaveable(id, target, preset != null) {
        mutableIntStateOf(preferredPort)
    }
    var reconnect by remember(id, target) { mutableIntStateOf(0) }
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    val window = LocalActivity.current?.window
    var foreground by remember(lifecycle) { mutableStateOf(lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)) }
    val closeEvents by rememberUpdatedState(onCloseEvents)

    if (hasEvents && !preview) {
        DisposableEffect(window) {
            val flag = WindowManager.LayoutParams.FLAG_SECURE
            val added = window != null && (window.attributes.flags and flag) == 0
            if (added) window?.addFlags(flag)
            onDispose {
                if (added) window?.clearFlags(flag)
            }
        }
        DisposableEffect(lifecycle, id, target) {
            val observer = LifecycleEventObserver { _, event ->
                when (event) {
                    Lifecycle.Event.ON_START -> foreground = true
                    Lifecycle.Event.ON_STOP -> {
                        foreground = false
                        closeEvents()
                    }
                    else -> Unit
                }
            }
            lifecycle.addObserver(observer)
            onDispose {
                lifecycle.removeObserver(observer)
                closeEvents()
            }
        }
        LaunchedEffect(id, target, session, appliedPort, usesSocket, reconnect, foreground, inScope) {
            if (inScope && target != null && session != null && foreground) {
                onListen(target, id, appliedPort.takeIf { usesSocket })
            } else {
                closeEvents()
            }
        }
    }
    val currentEvents = events?.takeIf { foreground && it.matches(target, id, session) }

    if (preset != null) {
        val restartListener = {
            if (session != null) {
                closeEvents()
                reconnect++
            }
        }
        PluginPresetLayout(
            preset = preset,
            plugin = plugin,
            target = target,
            busy = busy,
            inScope = inScope,
            working = working,
            runtimeError = runtimeError,
            targetOffline = targetOffline,
            targetInHeader = hoistHeader,
            events = currentEvents,
            usesSocket = usesSocket,
            preferredPort = appliedPort,
            onCheck = onCheck,
            onStart = { if (target != null) onStart(target, id) },
            onStop = { if (target != null) onStop(target, id) },
            onCall = { action -> if (target != null) onCall(target, id, action) },
            onReconnect = restartListener,
            onApplyPort = {
                appliedPort = it
                onSetPreferredPort(it)
                restartListener()
            },
        )
        return
    }

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(start = 16.dp, top = 8.dp, end = 16.dp, bottom = 24.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        if (!hoistHeader) item {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                IconButton(onClick = onBack) {
                    Icon(Icons.AutoMirrored.Rounded.ArrowBack, contentDescription = stringResource(R.string.navigate_back))
                }
                SectionTitle(
                    title = page.title.localized(),
                    subtitle = target ?: stringResource(R.string.package_no_target),
                    modifier = Modifier.weight(1f),
                )
            }
        }
        if (working) {
            item {
                Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    LinearProgressIndicator(Modifier.fillMaxWidth())
                    Text(stringResource(R.string.package_working), style = MaterialTheme.typography.bodySmall)
                }
            }
        }
        runtimeError?.let { error -> item { if (targetOffline) NoticeBanner(error) else ErrorBanner(error) } }
        plugin.error?.let { error -> item { ErrorBanner(error) } }
        for (block in page.blocks) {
            when (block) {
                is PluginPageBlock.Text -> item {
                    Text(block.text.localized(), color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                PluginPageBlock.Status -> item {
                    LiveStatus(
                        label = stringResource(plugin.runState.labelRes),
                        live = plugin.runState == DirectoryPluginState.RUNNING,
                        error = plugin.runState == DirectoryPluginState.ERROR,
                    )
                }
                is PluginPageBlock.Button -> item {
                    val available = when (block.action) {
                        "check" -> true
                        "start" -> inScope && plugin.runState == DirectoryPluginState.STOPPED
                        "stop" -> plugin.runState == DirectoryPluginState.RUNNING ||
                            plugin.runState == DirectoryPluginState.ERROR
                        "call" -> inScope && session != null && block.call != null && (!usesSocket || currentEvents?.connected == true)
                        else -> false
                    }
                    val enabled = target != null && !busy && available
                    val onClick = {
                        if (target != null) {
                            when (block.action) {
                                "check" -> onCheck()
                                "start" -> onStart(target, id)
                                "stop" -> onStop(target, id)
                                "call" -> block.call?.let { onCall(target, id, it) }
                                else -> Unit
                            }
                        }
                    }
                    if (block.action == "stop") {
                        OutlinedButton(
                            onClick = onClick,
                            enabled = enabled,
                            colors = ButtonDefaults.outlinedButtonColors(contentColor = MaterialTheme.colorScheme.error),
                            border = BorderStroke(
                                1.dp,
                                if (enabled) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.outlineVariant,
                            ),
                            modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
                        ) {
                            Text(block.label.localized())
                        }
                    } else {
                        Button(
                            onClick = onClick,
                            enabled = enabled,
                            colors = if (block.action == "start") ButtonDefaults.buttonColors()
                            else ButtonDefaults.filledTonalButtonColors(),
                            modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
                        ) {
                            Text(block.label.localized())
                        }
                    }
                }
                is PluginPageBlock.Input -> item {
                    val validPort = portDraft.toIntOrNull()?.takeIf(::validPluginListenPort)
                    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        OutlinedTextField(
                            value = portDraft,
                            onValueChange = { value ->
                                if (value.length <= 5 && value.all { it in '0'..'9' }) portDraft = value
                            },
                            label = { Text(block.label.localized()) },
                            supportingText = {
                                Text(stringResource(
                                    if (validPort == null) R.string.package_page_invalid_port
                                    else R.string.package_page_port_hint,
                                ))
                            },
                            isError = validPort == null,
                            singleLine = true,
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                            modifier = Modifier.fillMaxWidth(),
                        )
                        OutlinedButton(
                            enabled = inScope && validPort != null && !busy,
                            onClick = {
                                validPort?.let {
                                    appliedPort = it
                                    onSetPreferredPort(it)
                                    if (session != null) {
                                        closeEvents()
                                        reconnect++
                                    }
                                }
                            },
                            modifier = Modifier.heightIn(min = 48.dp),
                        ) {
                            Text(stringResource(R.string.package_page_apply_port))
                        }
                    }
                }
                is PluginPageBlock.Events -> {
                    item { PluginPageEventStatus(currentEvents, waitingForStart = session == null) }
                    items(currentEvents?.events.orEmpty().takeLast(block.limit).asReversed(), key = { it.id }) { event ->
                        PluginEventCard(event)
                    }
                }
            }
        }
    }
}

@Composable
private fun PluginPageEventStatus(events: PackageEventsUi?, waitingForStart: Boolean) {
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        SectionTitle(
            title = stringResource(R.string.package_live_events),
            subtitle = stringResource(R.string.package_events_memory_only),
        )
        LiveStatus(
            label = stringResource(
                when {
                    waitingForStart -> R.string.package_page_events_start_first
                    events?.error != null -> R.string.package_events_disconnected
                    events?.connected == true -> R.string.package_events_listening
                    else -> R.string.package_events_connecting
                },
            ),
            live = events?.connected == true,
            error = events?.error != null,
        )
        events?.listenPort?.let { port ->
            Text(stringResource(R.string.package_page_listen_address, port), style = MaterialTheme.typography.bodySmall)
        }
        events?.error?.let { error -> ErrorBanner(error) }
        if (events?.connected == true && events.events.isEmpty()) {
            Text(
                stringResource(R.string.package_events_empty_title),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                style = MaterialTheme.typography.bodySmall,
            )
        }
    }
}

@Composable
internal fun PluginPageText.localized(): String =
    if (LocalConfiguration.current.locales[0]?.language == "zh") zh ?: en else en
