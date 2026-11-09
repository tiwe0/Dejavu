package cafe.ivory.dejavu.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Android
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import cafe.ivory.dejavu.R
import cafe.ivory.dejavu.data.NativeLibraryPluginUi

@Composable
fun NativeLibraryPluginCard(
    item: NativeLibraryPluginUi,
    target: String?,
    busy: Boolean,
    working: Boolean,
    onLoad: () -> Unit,
    onUnload: () -> Unit,
    onSkill: () -> Unit,
) {
    val runtime = item.runtime
    val loaded = runtime?.loaded == true
    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.medium,
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        border = CardDefaults.outlinedCardBorder(),
    ) {
        Column(Modifier.padding(horizontal = 12.dp, vertical = 8.dp)) {
            val kind = stringResource(
                if (item.artifact.jit) R.string.native_library_kind_jit
                else R.string.native_library_kind_native,
            )
            PluginCardHeader(
                icon = Icons.Rounded.Android,
                title = item.artifact.name,
                metadata = if (item.artifact.version.isBlank()) kind else "$kind · ${item.artifact.version}",
                status = stringResource(
                    when {
                        !item.runtimeKnown -> R.string.package_state_unknown
                        loaded -> R.string.status_loaded
                        else -> R.string.native_library_not_loaded
                    },
                ),
                active = loaded,
            )
            if (item.artifact.description.isNotBlank()) {
                Text(
                    text = item.artifact.description,
                    modifier = Modifier.padding(bottom = 4.dp),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            SelectionContainer {
                Text(
                    text = item.artifact.path,
                    modifier = Modifier.padding(bottom = 8.dp),
                    style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            item.error?.let { error ->
                Text(
                    error,
                    modifier = Modifier.padding(bottom = 8.dp),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                )
            }
            if (working) {
                LinearProgressIndicator(Modifier.fillMaxWidth())
                Text(
                    stringResource(R.string.package_working),
                    modifier = Modifier.padding(vertical = 4.dp),
                    style = MaterialTheme.typography.bodySmall,
                )
            }
            if (target == null) {
                Text(
                    stringResource(R.string.package_no_target),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            HorizontalDivider()
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.End,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                if (runtime?.skill == true) {
                    TextButton(
                        onClick = onSkill,
                        enabled = target != null && !busy,
                        modifier = Modifier.weight(1f, fill = false).heightIn(min = 48.dp),
                    ) { Text(stringResource(R.string.action_skill)) }
                }
                TextButton(
                    onClick = if (loaded) onUnload else onLoad,
                    enabled = target != null && !busy,
                    modifier = Modifier.weight(1f, fill = false).heightIn(min = 48.dp),
                ) { Text(stringResource(if (loaded) R.string.unload else R.string.load)) }
            }
        }
    }
}
