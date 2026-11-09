package cafe.ivory.dejavu.ui

import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.OpenInNew
import androidx.compose.material.icons.rounded.AccountTree
import androidx.compose.material.icons.rounded.Add
import androidx.compose.material.icons.rounded.Code
import androidx.compose.material.icons.rounded.Gavel
import androidx.compose.material.icons.rounded.Hub
import androidx.compose.material.icons.rounded.Security
import androidx.compose.material3.Button
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.res.colorResource
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import cafe.ivory.dejavu.R
import cafe.ivory.dejavu.data.ConsoleUiState

private const val PROJECT_URL = "https://github.com/tiwe0/Dejavu"

@Composable
internal fun OverviewScreen(
    state: ConsoleUiState,
    onSelectTarget: (String) -> Unit,
    onPickApp: () -> Unit,
    onManageScope: () -> Unit,
    onLinkUnavailable: (String) -> Unit,
) {
    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(start = 16.dp, top = 8.dp, end = 16.dp, bottom = 24.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        item { ModuleStatus(state) }
        item { OverviewTarget(state, onSelectTarget, onPickApp, onManageScope) }
        item {
            ManagerGroup {
                RuntimeStatus(
                    Icons.Rounded.Security,
                    stringResource(R.string.overview_root),
                    stringResource(R.string.overview_root_granted),
                    active = true,
                )
                HorizontalDivider()
                RuntimeStatus(
                    Icons.Rounded.Hub,
                    stringResource(R.string.overview_helper),
                    stringResource(
                        if (state.helperRunning) R.string.overview_running else R.string.overview_offline,
                    ),
                    active = state.helperRunning,
                    detail = state.helperPort.takeIf { state.helperRunning && it.isNotBlank() }
                        ?.let { stringResource(R.string.overview_helper_port, it) },
                )
            }
        }
        item { ProjectReferences(onLinkUnavailable) }
    }
}

@Composable
private fun ModuleStatus(state: ConsoleUiState) {
    val installed = state.moduleInstalled
    Surface(
        shape = MaterialTheme.shapes.large,
        color = if (installed) colorResource(R.color.ic_launcher_background)
        else MaterialTheme.colorScheme.surfaceVariant,
        contentColor = if (installed) Color(0xFFF8F3E7) else MaterialTheme.colorScheme.onSurfaceVariant,
    ) {
        Row(
            modifier = Modifier.fillMaxWidth().heightIn(min = 112.dp)
                .padding(horizontal = 12.dp, vertical = 10.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Image(
                painterResource(R.drawable.ic_launcher_foreground),
                contentDescription = null,
                modifier = Modifier.size(90.dp),
            )
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(
                    stringResource(if (installed) R.string.overview_module_detected else R.string.overview_module_missing),
                    style = MaterialTheme.typography.titleLarge,
                    fontWeight = FontWeight.SemiBold,
                )
                Text(
                    if (installed && state.moduleVersion.isNotBlank()) {
                        stringResource(R.string.module_version_format, state.moduleVersion)
                    } else if (installed) {
                        stringResource(R.string.module_product_name)
                    } else {
                        stringResource(R.string.module_missing_hint)
                    },
                    style = MaterialTheme.typography.bodyMedium,
                )
            }
        }
    }
}

@Composable
private fun OverviewTarget(
    state: ConsoleUiState,
    onSelectTarget: (String) -> Unit,
    onPickApp: () -> Unit,
    onManageScope: () -> Unit,
) {
    if (state.targets.isEmpty()) {
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(
                stringResource(R.string.no_targets_configured),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Button(onClick = onPickApp, shape = MaterialTheme.shapes.medium, modifier = Modifier.fillMaxWidth()) {
                Icon(Icons.Rounded.Add, contentDescription = null, modifier = Modifier.padding(end = 6.dp))
                Text(stringResource(R.string.add_target))
            }
        }
        return
    }
    val picker: @Composable (Modifier) -> Unit = { modifier ->
        TargetMenu(
            targets = state.targets,
            selected = state.selectedTarget,
            onSelect = onSelectTarget,
            modifier = modifier,
            apps = state.installedApps,
        )
    }
    val action: @Composable (Modifier) -> Unit = { modifier ->
        Button(
            onClick = onManageScope,
            modifier = modifier.heightIn(min = 48.dp),
            shape = MaterialTheme.shapes.medium,
            contentPadding = PaddingValues(horizontal = 12.dp, vertical = 8.dp),
        ) {
            Text(stringResource(R.string.overview_manage_scope))
        }
    }
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Text(
            stringResource(R.string.current_process),
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        BoxWithConstraints {
            if (maxWidth < 350.dp || LocalDensity.current.fontScale > 1.15f) {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    picker(Modifier.fillMaxWidth())
                    action(Modifier.fillMaxWidth())
                }
            } else {
                Row(
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    picker(Modifier.weight(1f))
                    action(Modifier)
                }
            }
        }
    }
}

@Composable
private fun RuntimeStatus(icon: ImageVector, label: String, status: String, active: Boolean, detail: String? = null) {
    Row(
        modifier = Modifier.fillMaxWidth().heightIn(min = 56.dp).padding(horizontal = 14.dp, vertical = 10.dp),
        horizontalArrangement = Arrangement.spacedBy(12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(icon, contentDescription = null, tint = MaterialTheme.colorScheme.primary)
        Column(Modifier.weight(1f)) {
            Text(label, style = MaterialTheme.typography.bodyMedium, fontWeight = FontWeight.Medium)
            if (detail != null) {
                Text(detail, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
        Surface(
            color = if (active) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant,
            contentColor = if (active) MaterialTheme.colorScheme.onPrimaryContainer
            else MaterialTheme.colorScheme.onSurfaceVariant,
            shape = MaterialTheme.shapes.extraLarge,
        ) {
            Text(status, Modifier.padding(horizontal = 10.dp, vertical = 4.dp), style = MaterialTheme.typography.labelMedium)
        }
    }
}

@Composable
private fun ProjectReferences(onLinkUnavailable: (String) -> Unit) {
    val uriHandler = LocalUriHandler.current
    val unavailable = stringResource(R.string.overview_link_unavailable)
    fun open(url: String) {
        try {
            uriHandler.openUri(url)
        } catch (_: IllegalArgumentException) {
            onLinkUnavailable(unavailable)
        }
    }
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        SectionTitle(stringResource(R.string.overview_project_title))
        ManagerGroup {
            ManagerLinkRow(
                Icons.Rounded.Code, stringResource(R.string.overview_source), "tiwe0/Dejavu",
                Icons.AutoMirrored.Rounded.OpenInNew,
            ) { open(PROJECT_URL) }
            HorizontalDivider()
            ManagerLinkRow(
                Icons.Rounded.AccountTree, stringResource(R.string.overview_references),
                stringResource(R.string.overview_reference_components),
                Icons.AutoMirrored.Rounded.OpenInNew,
            ) { open("$PROJECT_URL/blob/main/THIRD_PARTY_NOTICES.md") }
            HorizontalDivider()
            ManagerLinkRow(
                Icons.Rounded.Gavel, stringResource(R.string.overview_license),
                stringResource(R.string.overview_copyright),
                Icons.AutoMirrored.Rounded.OpenInNew,
            ) { open("$PROJECT_URL/blob/main/LICENSE") }
        }
    }
}

@Composable
internal fun ManagerGroup(content: @Composable () -> Unit) {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.large,
        color = MaterialTheme.colorScheme.surface,
        border = CardDefaults.outlinedCardBorder(),
    ) {
        Column { content() }
    }
}

@Composable
internal fun ManagerLinkRow(
    icon: ImageVector,
    label: String,
    detail: String,
    trailingIcon: ImageVector,
    onClick: () -> Unit,
) {
    Surface(onClick = onClick, modifier = Modifier.fillMaxWidth(), color = MaterialTheme.colorScheme.surface) {
        Row(
            modifier = Modifier.heightIn(min = 60.dp).padding(horizontal = 14.dp, vertical = 10.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(icon, contentDescription = null, tint = MaterialTheme.colorScheme.primary)
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(label, style = MaterialTheme.typography.titleSmall)
                Text(detail, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Icon(
                trailingIcon,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.size(18.dp),
            )
        }
    }
}
