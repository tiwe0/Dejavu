package cafe.ivory.dejavu.ui

import android.os.Build
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.rounded.Settings
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.Article
import androidx.compose.material.icons.automirrored.rounded.KeyboardArrowRight
import androidx.compose.material.icons.rounded.Check
import androidx.compose.material.icons.rounded.Brightness6
import androidx.compose.material.icons.rounded.Palette
import androidx.compose.material3.FilterChip
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.setValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import cafe.ivory.dejavu.ManagerAppearance
import cafe.ivory.dejavu.ManagerPalette
import cafe.ivory.dejavu.ManagerThemeMode
import cafe.ivory.dejavu.R
import cafe.ivory.dejavu.ui.theme.managerDisplayColorScheme

private enum class AppearancePanel { Mode, Palette }

@OptIn(ExperimentalLayoutApi::class, ExperimentalMaterial3Api::class)
@Composable
internal fun ConfigurationScreen(
    appearance: ManagerAppearance,
    onAppearanceChange: (ManagerAppearance) -> Unit,
    onOpenLogs: () -> Unit,
    onOpenRouter: () -> Unit,
) {
    val dark = when (appearance.mode) {
        ManagerThemeMode.SYSTEM -> isSystemInDarkTheme()
        ManagerThemeMode.LIGHT -> false
        ManagerThemeMode.DARK -> true
    }
    val palettes = ManagerPalette.entries.filter { it != ManagerPalette.SYSTEM || Build.VERSION.SDK_INT >= 31 }
    var panel by remember { mutableStateOf<AppearancePanel?>(null) }
    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(16.dp, 8.dp, 16.dp, 24.dp),
        verticalArrangement = Arrangement.spacedBy(20.dp),
    ) {
        item {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                SectionTitle(stringResource(R.string.configuration_appearance_section))
                ManagerGroup {
                    ManagerLinkRow(
                        Icons.Rounded.Brightness6,
                        stringResource(R.string.configuration_appearance),
                        stringResource(appearance.mode.labelRes),
                        Icons.AutoMirrored.Rounded.KeyboardArrowRight,
                        { panel = AppearancePanel.Mode },
                    )
                    HorizontalDivider()
                    ManagerLinkRow(
                        Icons.Rounded.Palette,
                        stringResource(R.string.configuration_colors),
                        stringResource(appearance.palette.labelRes),
                        Icons.AutoMirrored.Rounded.KeyboardArrowRight,
                        { panel = AppearancePanel.Palette },
                    )
                }
            }
        }
        item {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                SectionTitle(stringResource(R.string.configuration_diagnostics))
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
                        Icons.Rounded.Settings,
                        stringResource(R.string.router_title),
                        stringResource(R.string.router_summary),
                        Icons.AutoMirrored.Rounded.KeyboardArrowRight,
                        onOpenRouter,
                    )
                }
            }
        }
    }
    if (panel != null) {
        ModalBottomSheet(onDismissRequest = { panel = null }) {
            Column(
                Modifier.fillMaxWidth().verticalScroll(rememberScrollState())
                    .padding(start = 16.dp, end = 16.dp, bottom = 24.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                SectionTitle(stringResource(
                    if (panel == AppearancePanel.Mode) R.string.configuration_appearance else R.string.configuration_colors,
                ), stringResource(R.string.configuration_colors_hint))
                if (panel == AppearancePanel.Mode) {
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        ManagerThemeMode.entries.forEach { mode ->
                            FilterChip(
                                selected = appearance.mode == mode,
                                onClick = {
                                    onAppearanceChange(appearance.copy(mode = mode))
                                    panel = null
                                },
                                label = { Text(stringResource(mode.labelRes)) },
                                leadingIcon = if (appearance.mode == mode) {
                                    { Icon(Icons.Rounded.Check, contentDescription = null, modifier = Modifier.size(18.dp)) }
                                } else null,
                            )
                        }
                    }
                } else {
                    BoxWithConstraints {
                        val columns = if (maxWidth < 300.dp || LocalDensity.current.fontScale > 1.4f) 1 else 2
                        Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                            palettes.chunked(columns).forEach { row ->
                                Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                                    row.forEach { palette ->
                                        PaletteOption(
                                            palette = palette, dark = dark, active = appearance.palette == palette,
                                            onSelect = {
                                                onAppearanceChange(appearance.copy(palette = palette))
                                                panel = null
                                            },
                                            modifier = Modifier.weight(1f),
                                        )
                                    }
                                    if (row.size < columns) Spacer(Modifier.weight(1f))
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}


@Composable
private fun PaletteOption(
    palette: ManagerPalette,
    dark: Boolean,
    active: Boolean,
    onSelect: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val scheme = managerDisplayColorScheme(palette, dark)
    Surface(
        onClick = onSelect,
        modifier = modifier.semantics { role = Role.RadioButton; selected = active },
        shape = MaterialTheme.shapes.large,
        color = MaterialTheme.colorScheme.surface,
        border = BorderStroke(if (active) 2.dp else 1.dp,
            if (active) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.outlineVariant),
    ) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Surface(color = scheme.background, shape = MaterialTheme.shapes.medium) {
                Row(Modifier.fillMaxWidth().height(52.dp).padding(10.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Box(Modifier.size(30.dp).background(scheme.primary, CircleShape))
                    Box(Modifier.size(24.dp).background(scheme.primaryContainer, CircleShape))
                    Box(Modifier.size(18.dp).background(scheme.tertiary, CircleShape))
                }
            }
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(stringResource(palette.labelRes), modifier = Modifier.weight(1f),
                    style = MaterialTheme.typography.bodyMedium, fontWeight = if (active) FontWeight.SemiBold else FontWeight.Normal)
                if (active) Icon(Icons.Rounded.Check, contentDescription = stringResource(R.string.configuration_selected),
                    modifier = Modifier.size(18.dp), tint = MaterialTheme.colorScheme.primary)
            }
        }
    }
}
