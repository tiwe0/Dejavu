package io.dejavu.console.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.ui.draw.clip
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.Article
import androidx.compose.material.icons.rounded.Add
import androidx.compose.material.icons.rounded.Bolt
import androidx.compose.material.icons.rounded.CheckCircle
import androidx.compose.material.icons.rounded.DeleteOutline
import androidx.compose.material.icons.rounded.MoreVert
import androidx.compose.material.icons.rounded.Extension
import androidx.compose.material.icons.rounded.Hub
import androidx.compose.material.icons.rounded.Key
import androidx.compose.material.icons.rounded.Memory
import androidx.compose.material.icons.rounded.PlayArrow
import androidx.compose.material.icons.rounded.Refresh
import androidx.compose.material.icons.rounded.RestartAlt
import androidx.compose.material.icons.rounded.Search
import androidx.compose.material.icons.rounded.Security
import androidx.compose.material.icons.rounded.Stop
import androidx.compose.material.icons.rounded.Terminal
import androidx.compose.material.icons.rounded.Tune
import androidx.compose.material.icons.rounded.VerticalAlignBottom
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.RadioButton
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
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import io.dejavu.console.R
import io.dejavu.console.data.ConsoleUiState
import io.dejavu.console.data.PluginInfo
import kotlinx.coroutines.launch

@Composable
fun HomeScreen(
    state: ConsoleUiState,
    onRefresh: () -> Unit,
    onSelectTarget: (String) -> Unit,
    onNavigate: (Int) -> Unit,
    onRestartTarget: (String) -> Unit,
) {
    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(start = 16.dp, top = 8.dp, end = 16.dp, bottom = 104.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        if (!state.rootGranted) {
            item { ErrorBanner(stringResource(R.string.banner_root_required)) }
        }
        item {
            Card(
                shape = MaterialTheme.shapes.large,
                colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
                border = CardDefaults.outlinedCardBorder(),
            ) {
                Column(Modifier.padding(horizontal = 16.dp, vertical = 6.dp)) {
                    RuntimeRow(
                        label = stringResource(R.string.overview_module),
                        value = if (state.moduleInstalled) {
                            stringResource(R.string.module_version_format, state.moduleVersion)
                        } else {
                            stringResource(R.string.module_not_detected)
                        },
                        positive = state.moduleInstalled,
                    )
                    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                    RuntimeRow(
                        label = stringResource(R.string.root_shell_title),
                        value = if (state.rootGranted) {
                            stringResource(R.string.overview_root_granted)
                        } else {
                            stringResource(R.string.root_shell_unavailable)
                        },
                        positive = state.rootGranted,
                    )
                    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                    RuntimeRow(
                        label = stringResource(R.string.overview_helper),
                        value = if (state.helperRunning) {
                            stringResource(R.string.overview_helper_running, state.helperPort)
                        } else {
                            stringResource(R.string.status_offline)
                        },
                        positive = state.helperRunning,
                        monoValue = state.helperRunning,
                    )
                    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                    RuntimeRow(
                        label = stringResource(R.string.overview_target),
                        value = state.selectedTarget ?: stringResource(R.string.not_selected),
                        positive = state.selectedTarget != null,
                        monoValue = true,
                    )
                }
            }
        }
        if (!state.moduleInstalled) {
            item { Text(stringResource(R.string.module_missing_hint), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant) }
        }
        item {
            InfoCard(
                title = stringResource(R.string.active_target_title),
                subtitle = stringResource(R.string.active_target_subtitle),
            ) {
                if (state.targets.isEmpty()) {
                    Text(
                        stringResource(R.string.target_none_hint),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                } else {
                    TargetMenu(
                        targets = state.targets,
                        selected = state.selectedTarget,
                        onSelect = onSelectTarget,
                    )
                }
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = { onNavigate(1) }) {
                        Text(stringResource(R.string.edit_targets))
                    }
                    if (state.selectedTarget != null) {
                        OutlinedButton(onClick = { onRestartTarget(state.selectedTarget) }) {
                            Text(stringResource(R.string.restart))
                        }
                    }
                }
            }
        }
        item {
            Text(
                stringResource(R.string.helper_scope),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = 4.dp),
            )
        }
    }
}

@Composable
private fun RuntimeRow(
    label: String,
    value: String,
    positive: Boolean,
    monoValue: Boolean = false,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        Box(
            modifier = Modifier
                .size(6.dp)
                .clip(androidx.compose.foundation.shape.CircleShape)
                .background(
                    if (positive) MaterialTheme.colorScheme.primary
                    else MaterialTheme.colorScheme.outline,
                ),
        )
        Text(
            label,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.weight(1f),
        )
        Text(
            value,
            style = if (monoValue) MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace)
            else MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurface,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

@Composable
fun TargetsScreen(
    state: ConsoleUiState,
    onAdd: (String) -> Unit,
    onRemove: (String) -> Unit,
    onSelect: (String) -> Unit,
    onRestart: (String) -> Unit,
) {
    var input by rememberSaveable { mutableStateOf("") }
    var inputError by remember { mutableStateOf(false) }
    var pendingRemove by remember { mutableStateOf<String?>(null) }
    val focus = androidx.compose.ui.platform.LocalFocusManager.current
    val submit = {
        val name = input.trim()
        if (!name.matches(Regex("[A-Za-z0-9_.:-]{1,128}"))) {
            inputError = true
        } else {
            onAdd(name)
            input = ""
            inputError = false
            focus.clearFocus()
        }
    }
    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(16.dp, 8.dp, 16.dp, 104.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item {
            Column(Modifier.fillMaxWidth()) {
                Text(
                    stringResource(R.string.targets_intro_subtitle),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(Modifier.height(4.dp))
                Text(
                    stringResource(R.string.targets_intro_body),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        item {
            Card(
                shape = MaterialTheme.shapes.large,
                colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
                border = CardDefaults.outlinedCardBorder(),
            ) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedTextField(
                        value = input,
                        onValueChange = {
                            input = it
                            inputError = false
                        },
                        modifier = Modifier.fillMaxWidth(),
                        label = { Text(stringResource(R.string.target_process)) },
                        placeholder = { Text(stringResource(R.string.target_process_example)) },
                        singleLine = true,
                        isError = inputError,
                        supportingText = if (inputError) {
                            { Text(stringResource(R.string.msg_invalid_process)) }
                        } else {
                            null
                        },
                        trailingIcon = {
                            IconButton(onClick = submit, enabled = input.isNotBlank()) {
                                Icon(Icons.Rounded.Add, contentDescription = stringResource(R.string.add))
                            }
                        },
                        keyboardOptions = androidx.compose.foundation.text.KeyboardOptions(
                            imeAction = androidx.compose.ui.text.input.ImeAction.Done,
                        ),
                        keyboardActions = androidx.compose.foundation.text.KeyboardActions(
                            onDone = { submit() },
                        ),
                        shape = MaterialTheme.shapes.medium,
                    )
                }
            }
        }
        if (state.targets.isEmpty()) {
            item {
                EmptyState(
                    title = stringResource(R.string.targets_empty_title),
                    detail = stringResource(R.string.targets_empty_detail),
                    icon = { Icon(Icons.Rounded.Hub, contentDescription = null) },
                )
            }
        } else {
            item {
                Card(
                    shape = MaterialTheme.shapes.large,
                    colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
                    border = CardDefaults.outlinedCardBorder(),
                ) {
                    Column {
                        state.targets.forEachIndexed { index, target ->
                            TargetRow(
                                target = target,
                                selected = target == state.selectedTarget,
                                onSelect = { onSelect(target) },
                                onRestart = { onRestart(target) },
                                onRemove = { pendingRemove = target },
                            )
                            if (index != state.targets.lastIndex) {
                                HorizontalDivider(
                                    modifier = Modifier.padding(start = 56.dp),
                                    color = MaterialTheme.colorScheme.outlineVariant,
                                )
                            }
                        }
                    }
                }
            }
        }
    }

    if (pendingRemove != null) {
        AlertDialog(
            onDismissRequest = { pendingRemove = null },
            title = { Text(stringResource(R.string.dialog_remove_target_title)) },
            text = { Text(stringResource(R.string.dialog_remove_target_body, pendingRemove ?: "")) },
            confirmButton = {
                TextButton(
                    onClick = {
                        pendingRemove?.let(onRemove)
                        pendingRemove = null
                    },
                ) { Text(stringResource(R.string.action_remove)) }
            },
            dismissButton = {
                TextButton(onClick = { pendingRemove = null }) {
                    Text(stringResource(R.string.cancel))
                }
            },
        )
    }
}

@Composable
private fun TargetRow(
    target: String,
    selected: Boolean,
    onSelect: () -> Unit,
    onRestart: () -> Unit,
    onRemove: () -> Unit,
) {
    var menu by remember { mutableStateOf(false) }
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onSelect)
            .padding(start = 8.dp, top = 4.dp, end = 4.dp, bottom = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        RadioButton(selected = selected, onClick = onSelect)
        Column(Modifier.weight(1f).padding(vertical = 8.dp)) {
            Text(
                target,
                style = MaterialTheme.typography.bodyLarge,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Text(
                if (selected) stringResource(R.string.target_selected)
                else stringResource(R.string.target_tap_select),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Box {
            IconButton(onClick = { menu = true }) {
                Icon(
                    Icons.Rounded.MoreVert,
                    contentDescription = stringResource(R.string.action_more),
                )
            }
            DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                DropdownMenuItem(
                    text = { Text(stringResource(R.string.restart)) },
                    leadingIcon = { Icon(Icons.Rounded.RestartAlt, contentDescription = null) },
                    onClick = {
                        menu = false
                        onRestart()
                    },
                )
                DropdownMenuItem(
                    text = { Text(stringResource(R.string.action_remove)) },
                    leadingIcon = { Icon(Icons.Rounded.DeleteOutline, contentDescription = null) },
                    onClick = {
                        menu = false
                        onRemove()
                    },
                )
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PluginsScreen(
    state: ConsoleUiState,
    onRefresh: () -> Unit,
    onSelectTarget: (String) -> Unit,
    onUnload: (String) -> Unit,
    onLoad: (String) -> Unit,
    skillLoader: suspend (String) -> Result<String>,
) {
    var loadDialog by rememberSaveable { mutableStateOf(false) }
    var loadPath by rememberSaveable { mutableStateOf("") }
    var loadError by remember { mutableStateOf(false) }
    var skillPlugin by remember { mutableStateOf<PluginInfo?>(null) }
    var skillText by remember { mutableStateOf("") }
    var skillLoading by remember { mutableStateOf(false) }
    var pendingUnload by remember { mutableStateOf<String?>(null) }
    val scope = rememberCoroutineScope()
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    val skillFallback = stringResource(R.string.msg_skill_load_failed)
    val loadedCount = state.plugins.count { it.loaded }

    Column(Modifier.fillMaxSize()) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 8.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            if (state.targets.isEmpty()) {
                Text(
                    stringResource(R.string.target_none_hint),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.weight(1f),
                )
            } else {
                TargetMenu(
                    targets = state.targets,
                    selected = state.selectedTarget,
                    onSelect = onSelectTarget,
                    modifier = Modifier.weight(1f),
                )
            }
            IconButton(onClick = onRefresh, enabled = state.selectedTarget != null && !state.pluginsLoading) {
                Icon(Icons.Rounded.Refresh, contentDescription = stringResource(R.string.refresh_plugins))
            }
            IconButton(onClick = { loadDialog = true }, enabled = state.selectedTarget != null) {
                Icon(Icons.Rounded.Add, contentDescription = stringResource(R.string.load_plugin))
            }
        }
        if (state.pluginsLoading) {
            androidx.compose.material3.LinearProgressIndicator(
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp),
            )
        }
        LazyColumn(
            modifier = Modifier.fillMaxSize(),
            contentPadding = PaddingValues(start = 16.dp, end = 16.dp, bottom = 104.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            if (state.plugins.isNotEmpty()) {
                item {
                    Text(
                        stringResource(R.string.plugins_count, loadedCount) +
                            " / ${state.plugins.size}",
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
            if (state.plugins.isEmpty() && !state.pluginsLoading) {
                item {
                    EmptyState(
                        title = stringResource(R.string.plugins_no_status_title),
                        detail = stringResource(R.string.plugins_no_status_detail),
                        icon = { Icon(Icons.Rounded.Extension, contentDescription = null) },
                    )
                }
                item {
                    Button(
                        onClick = onRefresh,
                        enabled = state.selectedTarget != null,
                        modifier = Modifier.fillMaxWidth(),
                    ) { Text(stringResource(R.string.plugins_empty_action)) }
                }
            }
            items(state.plugins, key = { it.name }) { plugin ->
                PluginCard(
                    plugin = plugin,
                    onSkill = {
                        skillPlugin = plugin
                        skillText = ""
                        skillLoading = true
                        scope.launch {
                            skillText = skillLoader(plugin.name).fold(
                                onSuccess = { it },
                                onFailure = { it.message ?: skillFallback },
                            )
                            skillLoading = false
                        }
                    },
                    onUnload = { pendingUnload = plugin.name },
                )
            }
        }
    }

    if (loadDialog) {
        AlertDialog(
            onDismissRequest = { loadDialog = false },
            title = { Text(stringResource(R.string.load_plugin)) },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    OutlinedTextField(
                        value = loadPath,
                        onValueChange = {
                            loadPath = it
                            loadError = false
                        },
                        label = { Text(stringResource(R.string.plugin_path_label)) },
                        modifier = Modifier.fillMaxWidth(),
                        singleLine = true,
                        isError = loadError,
                        supportingText = if (loadError) {
                            { Text(stringResource(R.string.msg_invalid_plugin_path)) }
                        } else {
                            null
                        },
                    )
                }
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        val path = loadPath.trim()
                        if (!path.startsWith("/") || !path.endsWith(".so")) {
                            loadError = true
                        } else {
                            loadDialog = false
                            onLoad(path)
                        }
                    },
                ) { Text(stringResource(R.string.load)) }
            },
            dismissButton = {
                TextButton(onClick = { loadDialog = false }) {
                    Text(stringResource(R.string.cancel))
                }
            },
        )
    }

    if (pendingUnload != null) {
        AlertDialog(
            onDismissRequest = { pendingUnload = null },
            title = { Text(stringResource(R.string.dialog_unload_plugin_title)) },
            text = {
                Text(stringResource(R.string.dialog_unload_plugin_body, pendingUnload ?: ""))
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        pendingUnload?.let(onUnload)
                        pendingUnload = null
                    },
                ) { Text(stringResource(R.string.unload)) }
            },
            dismissButton = {
                TextButton(onClick = { pendingUnload = null }) {
                    Text(stringResource(R.string.cancel))
                }
            },
        )
    }

    if (skillPlugin != null) {
        ModalBottomSheet(
            onDismissRequest = { skillPlugin = null },
            sheetState = sheetState,
        ) {
            Column(
                Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 20.dp, vertical = 8.dp)
                    .verticalScroll(rememberScrollState()),
            ) {
                Text(skillPlugin!!.name, style = MaterialTheme.typography.titleLarge)
                Text(
                    stringResource(R.string.skill_md),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(Modifier.height(12.dp))
                if (skillLoading) {
                    CircularProgressIndicator(Modifier.size(24.dp))
                } else {
                    SelectionContainer {
                        Text(
                            skillText,
                            style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
                        )
                    }
                }
                Spacer(Modifier.height(32.dp))
            }
        }
    }
}

@Composable
private fun PluginCard(
    plugin: PluginInfo,
    onSkill: () -> Unit,
    onUnload: () -> Unit,
) {
    var menu by remember { mutableStateOf(false) }
    Card(
        shape = MaterialTheme.shapes.medium,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        border = CardDefaults.outlinedCardBorder(),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(start = 16.dp, top = 12.dp, end = 4.dp, bottom = 12.dp),
            verticalAlignment = Alignment.Top,
        ) {
            Column(Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        plugin.name,
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.SemiBold,
                    )
                    if (plugin.version.isNotBlank()) {
                        Spacer(Modifier.size(8.dp))
                        Text(
                            plugin.version,
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.primary,
                        )
                    }
                }
                Spacer(Modifier.height(4.dp))
                MonoText(plugin.path)
                Spacer(Modifier.height(8.dp))
                Row(
                    horizontalArrangement = Arrangement.spacedBy(14.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(5.dp),
                    ) {
                        Box(
                            Modifier
                                .size(6.dp)
                                .clip(CircleShape)
                                .background(
                                    if (plugin.loaded) MaterialTheme.colorScheme.primary
                                    else MaterialTheme.colorScheme.outline,
                                ),
                        )
                        Text(
                            if (plugin.loaded) stringResource(R.string.status_loaded)
                            else stringResource(R.string.status_idle),
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    Text(
                        stringResource(R.string.plugin_caps_format, plugin.capabilities),
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    if (plugin.jit) {
                        Text(
                            "JIT",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }
            Box {
                IconButton(onClick = { menu = true }) {
                    Icon(
                        Icons.Rounded.MoreVert,
                        contentDescription = stringResource(R.string.action_more),
                    )
                }
                DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                    if (plugin.skill) {
                        DropdownMenuItem(
                            text = { Text(stringResource(R.string.action_skill)) },
                            onClick = {
                                menu = false
                                onSkill()
                            },
                        )
                    }
                    DropdownMenuItem(
                        text = { Text(stringResource(R.string.unload)) },
                        onClick = {
                            menu = false
                            onUnload()
                        },
                    )
                }
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

@Composable
fun ConsoleScreen(
    state: ConsoleUiState,
    onSelectTarget: (String) -> Unit,
    runner: suspend (String) -> Result<String>,
) {
    var script by rememberSaveable { mutableStateOf("return plugin.list()") }
    var output by rememberSaveable { mutableStateOf("") }
    var running by remember { mutableStateOf(false) }
    var outputError by remember { mutableStateOf(false) }
    var templateMenu by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()
    val templates = listOf(
        stringResource(R.string.template_plugin_list) to "return plugin.list()",
        stringResource(R.string.template_algorithm_status) to
            "return plugin.call(\"algorithm\", \"status\")",
        stringResource(R.string.template_capture_status) to
            "return plugin.call(\"capture\", \"status\")",
        stringResource(R.string.template_native_list) to "return native.list()",
        stringResource(R.string.template_root_helper) to
            "return \"port=\" .. (prop.get(\"debug.dejavu.root.port\") or \"none\")",
    )
    val errorPrefix = stringResource(R.string.msg_error_prefix, "")
    val emptyResponse = stringResource(R.string.empty_response)
    val responseEmpty = stringResource(R.string.response_empty)

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(16.dp, 8.dp, 16.dp, 104.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item {
            if (state.targets.isEmpty()) {
                Text(
                    stringResource(R.string.target_none_hint),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            } else {
                TargetMenu(
                    targets = state.targets,
                    selected = state.selectedTarget,
                    onSelect = onSelectTarget,
                )
            }
        }
        item {
            Card(
                shape = MaterialTheme.shapes.large,
                colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
                border = CardDefaults.outlinedCardBorder(),
            ) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    Text(
                        stringResource(R.string.console_subtitle),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    OutlinedTextField(
                        value = script,
                        onValueChange = { script = it },
                        modifier = Modifier.fillMaxWidth(),
                        minLines = 4,
                        textStyle = MaterialTheme.typography.bodySmall.copy(
                            fontFamily = FontFamily.Monospace,
                        ),
                        label = { Text(stringResource(R.string.console_script_label)) },
                        shape = MaterialTheme.shapes.medium,
                    )
                    Row(
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Button(
                            onClick = {
                                running = true
                                output = ""
                                outputError = false
                                scope.launch {
                                    output = runner(script).fold(
                                        onSuccess = { it.ifBlank { emptyResponse } },
                                        onFailure = {
                                            outputError = true
                                            errorPrefix + it.message
                                        },
                                    )
                                    running = false
                                }
                            },
                            enabled = !running && state.selectedTarget != null,
                        ) {
                            if (running) {
                                CircularProgressIndicator(
                                    modifier = Modifier.size(16.dp),
                                    strokeWidth = 2.dp,
                                )
                                Spacer(Modifier.size(8.dp))
                            }
                            Text(
                                if (running) stringResource(R.string.running)
                                else stringResource(R.string.run),
                            )
                        }
                        TextButton(
                            onClick = {
                                output = ""
                                outputError = false
                            },
                            enabled = output.isNotBlank(),
                        ) { Text(stringResource(R.string.clear_output)) }
                    }
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(
                            stringResource(R.string.console_templates),
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.weight(1f),
                        )
                        Box {
                            TextButton(onClick = { templateMenu = true }) {
                                Text(stringResource(R.string.choose_template))
                            }
                            DropdownMenu(
                                expanded = templateMenu,
                                onDismissRequest = { templateMenu = false },
                            ) {
                                templates.forEach { template ->
                                    DropdownMenuItem(
                                        text = { Text(template.first) },
                                        onClick = {
                                            script = template.second
                                            templateMenu = false
                                        },
                                    )
                                }
                            }
                        }
                    }
                }
            }
        }
        item {
            Card(
                shape = MaterialTheme.shapes.large,
                colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
                border = CardDefaults.outlinedCardBorder(),
            ) {
                Column(Modifier.padding(16.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Icon(
                            Icons.Rounded.Terminal,
                            contentDescription = null,
                            tint = if (outputError) MaterialTheme.colorScheme.error
                            else MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.size(18.dp),
                        )
                        Spacer(Modifier.size(8.dp))
                        Text(
                            stringResource(R.string.response),
                            style = MaterialTheme.typography.titleSmall,
                            modifier = Modifier.weight(1f),
                        )
                        if (output.isNotBlank()) {
                            IconButton(onClick = {
                                output = ""
                                outputError = false
                            }) {
                                Icon(
                                    Icons.Rounded.DeleteOutline,
                                    contentDescription = stringResource(R.string.clear_output),
                                )
                            }
                        }
                    }
                    Spacer(Modifier.height(6.dp))
                    SelectionContainer {
                        Text(
                            output.ifBlank { responseEmpty },
                            style = MaterialTheme.typography.bodySmall.copy(
                                fontFamily = FontFamily.Monospace,
                            ),
                            color = if (outputError) MaterialTheme.colorScheme.error
                            else MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }
        }
    }
}
