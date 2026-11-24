package cafe.ivory.dejavu.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import cafe.ivory.dejavu.R
import cafe.ivory.dejavu.data.NativeLibraryPluginUi
import cafe.ivory.dejavu.ui.theme.mono

/** Native / JIT artifact details block paired with [RuntimeStateBlock] and the docked primary action. */
@Composable
internal fun NativeLibraryMetadataCard(
    item: NativeLibraryPluginUi,
    target: String?,
    busy: Boolean,
    onSkill: () -> Unit,
) {
    val runtime = item.runtime
    ManagerGroup {
        Column(Modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            TypeTag(
                (if (item.artifact.jit) "JIT" else "NATIVE") +
                    if (item.artifact.version.isBlank()) "" else " · ${item.artifact.version}",
            )
            if (item.artifact.description.isNotBlank()) {
                Text(
                    text = item.artifact.description,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            SelectionContainer {
                Text(
                    text = item.artifact.path,
                    style = MaterialTheme.typography.mono,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            if (target == null) {
                Text(
                    stringResource(R.string.package_no_target),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            if (runtime?.skill == true) {
                OutlinedButton(
                    onClick = onSkill,
                    enabled = target != null && !busy,
                    modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
                ) {
                    Text(stringResource(R.string.action_skill))
                }
            }
        }
    }
}
