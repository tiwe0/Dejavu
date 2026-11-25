package cafe.ivory.dejavu.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Add
import androidx.compose.material.icons.rounded.DeleteOutline
import androidx.compose.material.icons.rounded.Edit
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import cafe.ivory.dejavu.R
import cafe.ivory.dejavu.data.RouterRoute
import cafe.ivory.dejavu.data.RouterUiState
import cafe.ivory.dejavu.data.RouterUpstream
import cafe.ivory.dejavu.ui.theme.mono

@Composable
internal fun RouterScreen(
    state: RouterUiState,
    onSetRunning: (Boolean) -> Unit,
    onReconnect: (String?) -> Unit,
    onConfigure: (String, String, Int, String, String?) -> Unit,
    onResetUpstream: (String) -> Unit,
    onReloadPlugins: () -> Unit,
    onPutRoute: (RouterRoute) -> Unit,
    onDeleteRoute: (String) -> Unit,
) {
    var connectionEditor by remember { mutableStateOf<RouterUpstream?>(null) }
    var resetting by remember { mutableStateOf<RouterUpstream?>(null) }
    var editing by remember { mutableStateOf<RouterRoute?>(null) }
    var deleting by remember { mutableStateOf<RouterRoute?>(null) }
    val snapshot = state.snapshot
    val running = state.service?.running == true
    val canManage = running && snapshot != null && !state.busy && state.failure == null
    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(16.dp, 8.dp, 16.dp, 24.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        item(key = "service") {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                SectionTitle(title = stringResource(R.string.router_service), subtitle = stringResource(R.string.router_independent))
                ManagerGroup {
                    Box(Modifier.fillMaxWidth()) {
                        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            LiveStatus(
                                label = stringResource(
                                    when {
                                        !state.healthKnown && state.failure != null -> R.string.router_unknown
                                        state.service == null -> R.string.router_loading
                                        !state.service.installed -> R.string.router_unavailable
                                        running -> R.string.router_running
                                        else -> R.string.router_stopped
                                    },
                                ),
                                live = running && state.healthKnown,
                                error = !state.healthKnown && state.failure != null,
                            )
                            if (state.service?.installed == false) Text(stringResource(R.string.router_install_hint), style = MaterialTheme.typography.bodySmall)
                            if (state.service?.installed == true) {
                                Text(stringResource(if (state.service.enabled) R.string.router_boot_enabled else R.string.router_boot_disabled), style = MaterialTheme.typography.bodySmall)
                                Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.padding(top = 4.dp)) {
                                    val toggleEnabled = !state.busy && !state.loading && (state.healthKnown || running)
                                    if (running) {
                                        OutlinedButton(
                                            onClick = { onSetRunning(false) },
                                            enabled = toggleEnabled,
                                            modifier = Modifier.heightIn(min = 48.dp),
                                            colors = ButtonDefaults.outlinedButtonColors(contentColor = MaterialTheme.colorScheme.error),
                                            border = BorderStroke(
                                                1.dp,
                                                if (toggleEnabled) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.outlineVariant,
                                            ),
                                        ) { Text(stringResource(R.string.router_stop)) }
                                        OutlinedButton(
                                            onClick = { onReconnect(null) },
                                            enabled = canManage,
                                            modifier = Modifier.heightIn(min = 48.dp),
                                        ) { Text(stringResource(R.string.router_reconnect_all)) }
                                    } else {
                                        Button(
                                            onClick = { onSetRunning(true) },
                                            enabled = toggleEnabled,
                                            modifier = Modifier.heightIn(min = 48.dp),
                                        ) { Text(stringResource(R.string.router_start)) }
                                    }
                                }
                            }
                        }
                        if (state.loading || state.busy) {
                            LinearProgressIndicator(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .align(Alignment.TopCenter),
                            )
                        }
                    }
                }
                if (state.failure != null) ErrorBanner(stringResource(R.string.router_error))
            }
        }
        if (snapshot != null && running) {
            item(key = "connections") {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    SectionTitle(title = stringResource(R.string.router_connections), subtitle = stringResource(R.string.router_upstreams_hint))
                    ManagerGroup {
                        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            val pluginCount = pluralStringResource(R.plurals.count_plugins, snapshot.pluginCount, snapshot.pluginCount)
                            val pendingCount = pluralStringResource(R.plurals.count_pending_jobs, snapshot.pendingJobs, snapshot.pendingJobs)
                            Text("$pluginCount · $pendingCount", style = MaterialTheme.typography.bodySmall)
                            if (snapshot.pluginSocket.isNotBlank()) MonoText(stringResource(R.string.router_plugin_socket, snapshot.pluginSocket))
                            if (snapshot.listenTcp) Text(stringResource(R.string.router_compatibility_port, snapshot.listenPort), style = MaterialTheme.typography.bodySmall)
                            TextButton(onClick = onReloadPlugins, enabled = canManage) { Text(stringResource(R.string.router_reload_plugins)) }
                        }
                    }
                }
            }
            items(snapshot.upstreams, key = { "upstream:${it.id}" }) { upstream ->
                ManagerGroup {
                    Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                            Text(
                                if (upstream.isDefault) stringResource(R.string.router_default_hub) else upstream.id,
                                style = MaterialTheme.typography.titleSmall,
                                modifier = Modifier.weight(1f),
                            )
                            LiveStatus(
                                stringResource(if (upstream.connected) R.string.router_hub_online else R.string.router_hub_offline),
                                upstream.connected,
                            )
                        }
                        if (upstream.host.isNotBlank()) MonoText("${upstream.host}:${upstream.port}")
                        if (!upstream.isDefault) {
                            Text(
                                stringResource(R.string.router_source, upstream.source),
                                style = MaterialTheme.typography.mono,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                            Text(
                                stringResource(if (upstream.overridden) R.string.router_local_override else R.string.router_plugin_defaults),
                                style = MaterialTheme.typography.bodySmall,
                            )
                        }
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            TextButton(onClick = { connectionEditor = upstream }, enabled = canManage) { Text(stringResource(R.string.router_configure)) }
                            TextButton(onClick = { onReconnect(upstream.id) }, enabled = canManage) { Text(stringResource(R.string.router_reconnect)) }
                        }
                        if (!upstream.isDefault && upstream.overridden) TextButton(onClick = { resetting = upstream }, enabled = canManage) {
                            Text(stringResource(R.string.router_reset_override))
                        }
                    }
                }
            }
            item(key = "routes_header") {
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    SectionTitle(
                        title = stringResource(R.string.router_routes),
                        modifier = Modifier.weight(1f),
                        subtitle = stringResource(R.string.router_rules_hint),
                    )
                    IconButton(onClick = { editing = RouterRoute("") }, enabled = canManage) {
                        Icon(Icons.Rounded.Add, stringResource(R.string.router_add_route))
                    }
                }
            }
            if (snapshot.routes.isEmpty()) item(key = "routes_empty") {
                EmptyState(stringResource(R.string.router_no_routes), stringResource(R.string.router_no_routes_detail))
            }
            items(snapshot.routes, key = { "route:${it.route}" }) { route ->
                ManagerGroup {
                    Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                        Row(Modifier.fillMaxWidth()) {
                            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                                Text(route.route, style = MaterialTheme.typography.titleSmall)
                                LiveStatus(stringResource(if (route.online) R.string.router_route_online else R.string.router_route_offline), route.online)
                            }
                            IconButton(onClick = { editing = route }, enabled = canManage) {
                                Icon(Icons.Rounded.Edit, stringResource(R.string.router_edit_route))
                            }
                            if (route.configured) IconButton(onClick = { deleting = route }, enabled = canManage) {
                                Icon(Icons.Rounded.DeleteOutline, stringResource(R.string.router_delete_rule))
                            }
                        }
                        val target = route.packageName.ifBlank { route.app }
                        if (target.isNotBlank()) MonoText(target)
                        Text(stringResource(R.string.router_route_upstream, route.upstreamId), style = MaterialTheme.typography.bodySmall)
                        Text(stringResource(if (route.configured) R.string.router_rule_configured else R.string.router_live_only), style = MaterialTheme.typography.bodySmall)
                    }
                }
            }
        }
    }
    connectionEditor?.let { upstream -> ConnectionDialog(
        upstream.host, upstream.port, upstream.deviceId, upstream.tokenConfigured,
        allowEmptyHost = upstream.isDefault,
        enabled = canManage && upstream.configurationMatches(snapshot?.upstreams?.find { it.id == upstream.id }),
        configurationChanged = snapshot != null && !upstream.configurationMatches(snapshot.upstreams.find { it.id == upstream.id }),
        onDismiss = { connectionEditor = null }, onSave = { host, port, id, token ->
            connectionEditor = null
            onConfigure(upstream.id, host, port, id, token)
        },
    ) }
    resetting?.let { upstream -> AlertDialog(
        onDismissRequest = { resetting = null },
        title = { Text(stringResource(R.string.router_reset_override)) },
        text = { Text(stringResource(if (snapshot != null && !upstream.configurationMatches(snapshot.upstreams.find { it.id == upstream.id }))
            R.string.router_config_changed else R.string.router_reset_override_hint, upstream.id)) },
        confirmButton = { TextButton(enabled = canManage && upstream.configurationMatches(snapshot?.upstreams?.find { it.id == upstream.id }), onClick = {
            resetting = null
            onResetUpstream(upstream.id)
        }) { Text(stringResource(R.string.router_reset_override)) } },
        dismissButton = { TextButton(onClick = { resetting = null }) { Text(stringResource(R.string.router_cancel)) } },
    ) }
    editing?.let { route -> RouteDialog(route, enabled = canManage, onDismiss = { editing = null }, onSave = {
        editing = null
        onPutRoute(it)
    }) }
    deleting?.let { route -> AlertDialog(
        onDismissRequest = { deleting = null },
        title = { Text(stringResource(R.string.router_delete_rule)) },
        text = { Text(stringResource(R.string.router_delete_hint, route.route)) },
        confirmButton = { TextButton(enabled = canManage, onClick = { deleting = null; onDeleteRoute(route.route) }) { Text(stringResource(R.string.router_delete_rule)) } },
        dismissButton = { TextButton(onClick = { deleting = null }) { Text(stringResource(R.string.router_cancel)) } },
    ) }
}

@Composable
private fun ConnectionDialog(
    initialHost: String, initialPort: Int, initialId: String, tokenConfigured: Boolean, enabled: Boolean,
    configurationChanged: Boolean,
    allowEmptyHost: Boolean,
    onDismiss: () -> Unit, onSave: (String, Int, String, String?) -> Unit,
) {
    var host by remember { mutableStateOf(initialHost) }
    var port by remember { mutableStateOf(initialPort.toString()) }
    var deviceId by remember { mutableStateOf(initialId) }
    // Deliberately not saveable: credentials never survive this dialog or process recreation.
    var token by remember { mutableStateOf("") }
    val valid = (allowEmptyHost || host.isNotBlank()) && host.toByteArray().size <= 253 && deviceId.isNotBlank() && deviceId.toByteArray().size <= 128 && port.toIntOrNull() in 1..65535 && token.toByteArray().size <= 4096
    AlertDialog(
        onDismissRequest = { token = ""; onDismiss() },
        title = { Text(stringResource(R.string.router_connection)) },
        text = {
            FormColumn {
                Text(stringResource(when {
                    configurationChanged -> R.string.router_config_changed
                    allowEmptyHost -> R.string.router_connection_hint
                    else -> R.string.router_plugin_connection_hint
                }),
                    style = MaterialTheme.typography.bodySmall)
                FormField(host, { host = it }, R.string.router_hub_host)
                FormField(port, { port = it }, R.string.router_hub_port, number = true)
                FormField(deviceId, { deviceId = it }, R.string.router_device_id)
                OutlinedTextField(token, { token = it }, modifier = Modifier.fillMaxWidth(), singleLine = true,
                    label = { Text(stringResource(R.string.router_device_token)) }, visualTransformation = PasswordVisualTransformation(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                    supportingText = { Text(stringResource(if (tokenConfigured) R.string.router_token_keep else R.string.router_token_optional)) })
            }
        },
        confirmButton = { TextButton(enabled = valid && enabled, onClick = {
            val submitted = token.takeIf { it.isNotEmpty() }
            token = ""
            onSave(host.trim(), port.toInt(), deviceId.trim(), submitted)
        }) { Text(stringResource(R.string.router_save)) } },
        dismissButton = { TextButton(onClick = { token = ""; onDismiss() }) { Text(stringResource(R.string.router_cancel)) } },
    )
}

@Composable
private fun RouteDialog(route: RouterRoute, enabled: Boolean, onDismiss: () -> Unit, onSave: (RouterRoute) -> Unit) {
    var id by remember { mutableStateOf(route.route) }
    var user by remember { mutableStateOf(route.userId.toString()) }
    var packageName by remember { mutableStateOf(route.packageName.ifBlank { route.app }) }
    var activity by remember { mutableStateOf(route.activity) }
    var plugin by remember { mutableStateOf(route.pluginPath) }
    val valid = id.isNotBlank() && id.toByteArray().size <= 128 && user.toIntOrNull() in 0..999 &&
        packageName.isNotBlank() && packageName.toByteArray().size <= 256 && activity.isNotBlank() && activity.toByteArray().size <= 256 && plugin.toByteArray().size <= 1024 && (plugin.isBlank() || plugin.startsWith("/"))
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(if (route.configured) R.string.router_edit_route else R.string.router_add_route)) },
        text = { FormColumn {
            FormField(id, { id = it }, R.string.router_route_id, enabled = route.route.isBlank())
            FormField(user, { user = it }, R.string.router_user_id, number = true)
            FormField(packageName, { packageName = it }, R.string.router_package)
            FormField(activity, { activity = it }, R.string.router_activity)
            FormField(plugin, { plugin = it }, R.string.router_plugin_path)
        } },
        confirmButton = { TextButton(enabled = valid && enabled, onClick = {
            onSave(RouterRoute(id.trim(), configured = true, userId = user.toInt(), packageName = packageName.trim(), activity = activity.trim(), pluginPath = plugin.trim()))
        }) { Text(stringResource(R.string.router_save)) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.router_cancel)) } },
    )
}

@Composable
private fun FormColumn(content: @Composable () -> Unit) {
    Column(Modifier.fillMaxWidth().heightIn(max = 480.dp).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(10.dp)) { content() }
}

@Composable
private fun FormField(value: String, onChange: (String) -> Unit, label: Int, number: Boolean = false, enabled: Boolean = true) {
    OutlinedTextField(value, onChange, Modifier.fillMaxWidth(), enabled = enabled, singleLine = true,
        label = { Text(stringResource(label)) }, keyboardOptions = KeyboardOptions(keyboardType = if (number) KeyboardType.Number else KeyboardType.Text))
}
