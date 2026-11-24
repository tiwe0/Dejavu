package cafe.ivory.dejavu.ui

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import cafe.ivory.dejavu.ManagerLanguage
import cafe.ivory.dejavu.R

@Composable
internal fun LanguageDialog(onDismiss: () -> Unit, onSelect: (String?) -> Unit) {
    val current = ManagerLanguage.current(LocalContext.current)
    val options = listOf(
        null to stringResource(R.string.language_system),
        "en" to stringResource(R.string.language_english),
        "zh-CN" to stringResource(R.string.language_chinese),
    )
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.language_title)) },
        text = {
            Column(Modifier.selectableGroup()) {
                options.forEach { (tag, label) ->
                    val selected = tag == current
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .heightIn(min = 48.dp)
                            .selectable(selected, role = Role.RadioButton, onClick = { onSelect(tag) }),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        RadioButton(
                            selected = selected,
                            onClick = null,
                            modifier = Modifier.padding(start = 12.dp, end = 16.dp),
                        )
                        Text(label)
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) }
        },
    )
}
