package cafe.ivory.dejavu.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.Article
import androidx.compose.material.icons.rounded.DeleteOutline
import androidx.compose.material.icons.rounded.PlayArrow
import androidx.compose.material.icons.rounded.Search
import androidx.compose.material.icons.rounded.Stop
import androidx.compose.material.icons.rounded.VerticalAlignBottom
import androidx.compose.material3.FilledTonalIconToggleButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import cafe.ivory.dejavu.R
import cafe.ivory.dejavu.data.LogsUiState
import cafe.ivory.dejavu.ui.theme.mono

@Composable
internal fun LogsScreen(
    state: LogsUiState,
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
    LaunchedEffect(state.logVersion, lines.size, autoScroll) {
        if (autoScroll && lines.isNotEmpty()) listState.scrollToItem(lines.lastIndex)
    }
    Column(Modifier.fillMaxSize().imePadding()) {
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
            IconButton(onClick = if (state.running) onStop else onStart) {
                Icon(
                    if (state.running) Icons.Rounded.Stop else Icons.Rounded.PlayArrow,
                    contentDescription = stringResource(if (state.running) R.string.logs_stop else R.string.logs_start),
                )
            }
            FilledTonalIconToggleButton(
                checked = autoScroll,
                onCheckedChange = { autoScroll = it },
            ) {
                Icon(
                    Icons.Rounded.VerticalAlignBottom,
                    contentDescription = stringResource(R.string.logs_auto_scroll),
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
                .padding(horizontal = 16.dp, vertical = 2.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                pluralStringResource(R.plurals.logs_lines, lines.size, lines.size),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.weight(1f),
            )
            if (state.running) {
                LiveStatus(stringResource(R.string.status_running), live = true)
            }
        }
        if (lines.isEmpty()) {
            EmptyState(
                title = when {
                    filter.isNotBlank() -> stringResource(R.string.logs_no_matches)
                    state.running -> stringResource(R.string.logs_empty_running_title)
                    else -> stringResource(R.string.logs_empty_title)
                },
                detail = when {
                    filter.isNotBlank() -> stringResource(R.string.logs_filter_empty_detail)
                    state.running -> stringResource(R.string.logs_empty_running_detail)
                    else -> stringResource(R.string.logs_empty_detail)
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
                            style = MaterialTheme.typography.mono,
                            color = when {
                                line.contains(" E ") || line.contains("FATAL") -> MaterialTheme.colorScheme.error
                                line.contains(" W ") -> MaterialTheme.colorScheme.tertiary
                                else -> MaterialTheme.colorScheme.onSurfaceVariant
                            },
                        )
                    }
                }
            }
        }
    }
}
