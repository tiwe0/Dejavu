package cafe.ivory.dejavu.ui

import androidx.compose.foundation.background
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
import androidx.compose.foundation.selection.toggleable
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Search
import androidx.compose.material3.Button
import androidx.compose.material3.Checkbox
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import cafe.ivory.dejavu.R
import cafe.ivory.dejavu.data.InstalledApp
import cafe.ivory.dejavu.ui.theme.DejavuMono
import cafe.ivory.dejavu.ui.theme.mono

@Composable
internal fun PluginScopeScreen(
    id: String,
    savedScope: List<String>,
    selected: List<String>,
    customTargets: List<String>,
    apps: List<InstalledApp>,
    appsByPackage: Map<String, InstalledApp>,
    loading: Boolean,
    saving: Boolean,
    enabled: Boolean,
    onSelectedChange: (List<String>) -> Unit,
    onAddCustomTarget: (String) -> Unit,
    onLoadApps: () -> Unit,
    onSave: (List<String>) -> Unit,
) {
    var query by rememberSaveable(id) { mutableStateOf("") }
    var process by rememberSaveable(id) { mutableStateOf("") }
    var processError by rememberSaveable(id) { mutableStateOf(false) }
    LaunchedEffect(id) { onLoadApps() }

    val savedSet = remember(savedScope) { savedScope.toSet() }
    val customSet = remember(customTargets) { customTargets.toSet() }
    val selectedSet = remember(selected) { selected.toSet() }
    val dirty = remember(selectedSet, savedSet) { selectedSet != savedSet }

    val targets = remember(customTargets, savedScope, apps, appsByPackage, query, savedSet, customSet) {
        (customTargets + savedScope + apps.map { it.packageName })
            .distinct()
            .filter { target ->
                query.isBlank() || target.contains(query, ignoreCase = true) ||
                    appsByPackage[target.substringBefore(':')]?.label?.contains(query, ignoreCase = true) == true
            }
            .sortedWith(
                // Pin newly added unsaved custom processes first, then saved scope, then alphabetical.
                compareBy<String>(
                    { it !in customSet },
                    { it !in savedSet },
                    { appsByPackage[it.substringBefore(':')]?.label?.lowercase() ?: it.lowercase() },
                ),
            )
    }

    Column(Modifier.fillMaxSize()) {
        LazyColumn(
            modifier = Modifier.weight(1f),
            contentPadding = PaddingValues(horizontal = 16.dp, vertical = 8.dp),
            verticalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            item(key = "scope-controls") {
                Column(
                    Modifier.padding(bottom = 8.dp),
                    verticalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    Text(
                        stringResource(R.string.plugin_scope_hint),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    OutlinedTextField(
                        value = query,
                        onValueChange = { query = it },
                        singleLine = true,
                        modifier = Modifier.fillMaxWidth(),
                        leadingIcon = { Icon(Icons.Rounded.Search, contentDescription = null) },
                        label = { Text(stringResource(R.string.search_apps)) },
                    )
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        OutlinedTextField(
                            value = process,
                            onValueChange = { process = it; processError = false },
                            modifier = Modifier.weight(1f),
                            enabled = enabled,
                            singleLine = true,
                            isError = processError,
                            label = {
                                Text(
                                    stringResource(R.string.plugin_scope_custom_process),
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis,
                                )
                            },
                            placeholder = {
                                Text(
                                    stringResource(R.string.plugin_scope_custom_process_placeholder),
                                    style = MaterialTheme.typography.mono,
                                )
                            },
                            textStyle = MaterialTheme.typography.bodyMedium.copy(fontFamily = DejavuMono),
                            supportingText = if (processError) {
                                { Text(stringResource(R.string.msg_invalid_process)) }
                            } else null,
                        )
                        TextButton(
                            enabled = enabled,
                            modifier = Modifier.heightIn(min = 48.dp),
                            onClick = {
                                val value = process.trim()
                                if (!value.matches(Regex("[A-Za-z0-9_.:-]{1,128}"))) {
                                    processError = true
                                } else {
                                    onAddCustomTarget(value)
                                    process = ""
                                    query = ""
                                }
                            },
                        ) { Text(stringResource(R.string.plugin_scope_add_process)) }
                    }
                    if (loading || saving) LinearProgressIndicator(Modifier.fillMaxWidth())
                }
            }
            if (targets.isEmpty() && !loading) {
                item(key = "no-apps") {
                    Text(stringResource(R.string.no_apps), color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
            items(targets, key = { it }) { target ->
                val pkg = target.substringBefore(':')
                val app = appsByPackage[pkg]
                val checked = target in selectedSet
                val isNewCustom = target in customSet && target !in savedSet
                Row(
                    Modifier
                        .fillMaxWidth()
                        .heightIn(min = 60.dp)
                        .toggleable(
                            value = checked,
                            enabled = enabled,
                            role = Role.Checkbox,
                            onValueChange = { value ->
                                val next = if (value) (selected + target).distinct()
                                else selected.filterNot { it == target }
                                onSelectedChange(next)
                            },
                        )
                        .padding(vertical = 8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    Checkbox(checked = checked, onCheckedChange = null, enabled = enabled)
                    ProcessIcon(pkg)
                    Column(Modifier.weight(1f)) {
                        Row(
                            horizontalArrangement = Arrangement.spacedBy(6.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            if (app != null) {
                                Text(
                                    app.label,
                                    style = MaterialTheme.typography.bodyLarge,
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis,
                                    modifier = Modifier.weight(1f, fill = false),
                                )
                            }
                            if (isNewCustom) {
                                TypeTag(stringResource(R.string.plugin_scope_unsaved_tag))
                            }
                        }
                        Text(
                            target,
                            style = MaterialTheme.typography.mono,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            maxLines = 2,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                }
            }
        }
        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
        Box(
            Modifier
                .fillMaxWidth()
                .background(MaterialTheme.colorScheme.surface)
                .padding(horizontal = 16.dp, vertical = 12.dp),
        ) {
            Button(
                onClick = { onSave(selected.distinct().sorted()) },
                enabled = enabled && dirty,
                modifier = Modifier.fillMaxWidth().heightIn(min = 52.dp),
            ) {
                Text(
                    if (saving) stringResource(R.string.plugin_scope_saving)
                    else stringResource(R.string.plugin_scope_save, selected.size),
                )
            }
        }
    }
}
