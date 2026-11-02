package cafe.ivory.dejavu.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
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
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(
                    text = item.artifact.name,
                    modifier = Modifier.weight(1f),
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                )
                Text(
                    text = stringResource(
                        if (item.artifact.jit) R.string.native_library_kind_jit
                        else R.string.native_library_kind_native,
                    ),
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.primary,
                )
            }
            if (item.artifact.version.isNotBlank()) {
                Text(item.artifact.version, style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            if (item.artifact.description.isNotBlank()) {
                Text(item.artifact.description, style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            SelectionContainer {
                Text(
                    text = item.artifact.path,
                    style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            StatusPill(
                label = stringResource(
                    when {
                        !item.runtimeKnown -> R.string.package_state_unknown
                        loaded -> R.string.status_loaded
                        else -> R.string.native_library_not_loaded
                    },
                ),
                active = loaded,
                activeColor = MaterialTheme.colorScheme.primary,
            )
            item.error?.let { error ->
                Text(error, style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.error)
            }
            if (working) {
                LinearProgressIndicator(Modifier.fillMaxWidth())
                Text(stringResource(R.string.package_working), style = MaterialTheme.typography.bodySmall)
            }
            if (target == null) {
                Text(
                    stringResource(R.string.package_no_target),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                if (loaded) {
                    OutlinedButton(
                        onClick = onUnload,
                        enabled = target != null && !busy,
                        modifier = Modifier.weight(1f).heightIn(min = 48.dp),
                    ) { Text(stringResource(R.string.unload)) }
                } else {
                    Button(
                        onClick = onLoad,
                        enabled = target != null && !busy,
                        modifier = Modifier.weight(1f).heightIn(min = 48.dp),
                    ) { Text(stringResource(R.string.load)) }
                }
                if (runtime?.skill == true) {
                    OutlinedButton(
                        onClick = onSkill,
                        enabled = target != null && !busy,
                        modifier = Modifier.weight(1f).heightIn(min = 48.dp),
                    ) { Text(stringResource(R.string.action_skill)) }
                }
            }
        }
    }
}
