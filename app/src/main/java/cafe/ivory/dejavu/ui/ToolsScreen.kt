package cafe.ivory.dejavu.ui

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.Article
import androidx.compose.material.icons.automirrored.rounded.KeyboardArrowRight
import androidx.compose.material.icons.rounded.Terminal
import androidx.compose.material3.HorizontalDivider
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import cafe.ivory.dejavu.R

@Composable
internal fun ToolsScreen(target: String?, onOpenLogs: () -> Unit, onOpenLua: () -> Unit) {
    Column(
        modifier = Modifier.fillMaxSize().verticalScroll(rememberScrollState())
            .padding(horizontal = 16.dp, vertical = 8.dp),
    ) {
        ManagerGroup {
            ManagerLinkRow(
                Icons.AutoMirrored.Rounded.Article,
                stringResource(R.string.title_logs),
                stringResource(R.string.tools_logs_detail),
                Icons.AutoMirrored.Rounded.KeyboardArrowRight,
                onOpenLogs,
            )
            HorizontalDivider()
            ManagerLinkRow(
                Icons.Rounded.Terminal,
                stringResource(R.string.title_console),
                target ?: stringResource(R.string.not_selected),
                Icons.AutoMirrored.Rounded.KeyboardArrowRight,
                onOpenLua,
            )
        }
    }
}
