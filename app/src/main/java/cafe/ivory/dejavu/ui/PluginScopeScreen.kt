package cafe.ivory.dejavu.ui

import androidx.compose.foundation.layout.Arrangement
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
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import cafe.ivory.dejavu.R
import cafe.ivory.dejavu.data.InstalledApp

@Composable
internal fun PluginScopeScreen(
    id: String,
    scope: List<String>,
    apps: List<InstalledApp>,
    loading: Boolean,
    saving: Boolean,
    enabled: Boolean,
    onLoadApps: () -> Unit,
    onSave: (List<String>) -> Unit,
) {
    var selected by rememberSaveable(id, scope) { mutableStateOf(scope) }
    var customTargets by rememberSaveable(id, scope) { mutableStateOf(scope) }
    var query by rememberSaveable(id) { mutableStateOf("") }
    var process by rememberSaveable(id) { mutableStateOf("") }
    var processError by rememberSaveable(id) { mutableStateOf(false) }
    LaunchedEffect(id) { onLoadApps() }
    val appsByPackage = apps.associateBy { it.packageName }
    val targets = (customTargets + apps.map { it.packageName }).distinct().filter { target ->
        query.isBlank() || target.contains(query, true) ||
            appsByPackage[target.substringBefore(':')]?.label?.contains(query, true) == true
    }.sortedWith(compareBy<String> { it !in scope }.thenBy { appsByPackage[it]?.label ?: it })
    Column(Modifier.fillMaxSize()) {
        Column(Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Text(stringResource(R.string.plugin_scope_hint), style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            OutlinedTextField(value = query, onValueChange = { query = it }, singleLine = true,
                modifier = Modifier.fillMaxWidth(),
                leadingIcon = { Icon(Icons.Rounded.Search, contentDescription = null) },
                label = { Text(stringResource(R.string.search_apps)) })
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(value = process, onValueChange = { process = it; processError = false },
                    modifier = Modifier.weight(1f), enabled = enabled, singleLine = true,
                    isError = processError, label = { Text(stringResource(R.string.plugin_scope_custom_process)) },
                    supportingText = if (processError) {{ Text(stringResource(R.string.msg_invalid_process)) }} else null)
                TextButton(enabled = enabled, onClick = {
                    val value = process.trim()
                    if (!value.matches(Regex("[A-Za-z0-9_.:-]{1,128}"))) processError = true
                    else {
                        customTargets = (customTargets + value).distinct()
                        selected = (selected + value).distinct()
                        process = ""
                        query = ""
                    }
                }) { Text(stringResource(R.string.plugin_scope_add_process)) }
            }
            Button(onClick = { onSave(selected.distinct().sorted()) },
                enabled = enabled && selected.toSet() != scope.toSet(),
                modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp)) {
                Text(stringResource(if (saving) R.string.plugin_scope_saving else R.string.plugin_scope_save, selected.size))
            }
            if (loading || saving) LinearProgressIndicator(Modifier.fillMaxWidth())
        }
        LazyColumn(modifier = Modifier.weight(1f), contentPadding = PaddingValues(horizontal = 16.dp, vertical = 8.dp)) {
            if (targets.isEmpty() && !loading) item {
                Text(stringResource(R.string.no_apps), color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            items(targets, key = { it }) { target ->
                val app = appsByPackage[target.substringBefore(':')]
                val checked = target in selected
                Row(Modifier.fillMaxWidth().heightIn(min = 64.dp)
                    .toggleable(value = checked, enabled = enabled, role = Role.Checkbox, onValueChange = { value ->
                        selected = if (value) (selected + target).distinct() else selected.filterNot { it == target }
                    }).padding(vertical = 8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    Checkbox(checked = checked, onCheckedChange = null, enabled = enabled)
                    ProcessIcon(app)
                    Column(Modifier.weight(1f)) {
                        if (app != null) Text(app.label, style = MaterialTheme.typography.bodyLarge)
                        Text(target, style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
            }
        }
    }
}
