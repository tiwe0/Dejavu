package cafe.ivory.dejavu.ui

import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Download
import androidx.compose.material3.Button
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import cafe.ivory.dejavu.R
import cafe.ivory.dejavu.data.ModuleUiState
import cafe.ivory.dejavu.data.RootProvider
import cafe.ivory.dejavu.ui.theme.mono

@Composable
internal fun ModuleInstallScreen(state: ModuleUiState, onInstall: () -> Unit) {
    val snapshot = state.snapshot
    val upToDate = state.bundledVersion != null && state.bundledVersion == snapshot?.installedVersion
    var showOutput by rememberSaveable { mutableStateOf(false) }
    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = PaddingValues(16.dp, 8.dp, 16.dp, 24.dp),
        verticalArrangement = Arrangement.spacedBy(18.dp),
    ) {
        item {
            ManagerGroup {
                Column(Modifier.fillMaxWidth().padding(18.dp), verticalArrangement = Arrangement.spacedBy(14.dp)) {
                    AnimatedContent(
                        targetState = when {
                            state.working -> R.string.module_install_progress
                            snapshot?.removalPending == true -> R.string.module_install_removing
                            state.loading -> R.string.module_install_checking
                            snapshot?.rebootPending == true -> R.string.module_install_pending
                            snapshot?.disabled == true -> R.string.module_install_disabled
                            state.bundledVersion == null -> R.string.module_install_bundle_missing
                            !state.compatible -> R.string.module_install_unsupported
                            snapshot?.provider == null -> R.string.module_install_unknown_provider
                            upToDate -> R.string.module_install_up_to_date
                            else -> R.string.module_install_ready
                        },
                        transitionSpec = { Motion.roll() },
                        label = "installation status",
                    ) { label ->
                        Text(stringResource(label), style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.SemiBold)
                    }
                    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        InstallValue(
                            stringResource(R.string.module_install_current),
                            snapshot?.installedVersion ?: stringResource(R.string.module_install_not_installed),
                        )
                        InstallValue(stringResource(R.string.module_install_bundled), state.bundledVersion ?: "—")
                    }
                    AnimatedVisibility(state.working || state.loading) {
                        LinearProgressIndicator(Modifier.fillMaxWidth())
                    }
                    if (snapshot?.removalPending == true) {
                        Text(stringResource(R.string.module_install_removing_hint), style = MaterialTheme.typography.bodyMedium)
                    } else if (state.working) {
                        Text(stringResource(R.string.module_install_progress_hint), style = MaterialTheme.typography.bodyMedium)
                    } else if (snapshot?.rebootPending == true) {
                        Text(stringResource(R.string.module_install_pending_detail), style = MaterialTheme.typography.bodyMedium)
                    } else if (upToDate) {
                        OutlinedButton(
                            onClick = onInstall,
                            enabled = state.canInstall,
                            modifier = Modifier.fillMaxWidth().heightIn(min = 52.dp),
                        ) {
                            Icon(Icons.Rounded.Download, null, Modifier.padding(end = 8.dp))
                            Text(stringResource(R.string.module_install_reinstall, state.bundledVersion ?: "—"))
                        }
                    } else {
                        Button(
                            onClick = onInstall,
                            enabled = state.canInstall,
                            modifier = Modifier.fillMaxWidth().heightIn(min = 52.dp),
                        ) {
                            Icon(Icons.Rounded.Download, null, Modifier.padding(end = 8.dp))
                            Text(stringResource(R.string.module_install_action, state.bundledVersion ?: "—"))
                        }
                    }
                }
            }
        }
        item {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                SectionTitle(stringResource(R.string.module_install_environment))
                ManagerGroup {
                    InstallEnvironment(
                        stringResource(R.string.module_install_provider),
                        when (snapshot?.provider) {
                            RootProvider.APATCH -> "APatch"
                            RootProvider.KERNELSU -> "KernelSU"
                            RootProvider.MAGISK -> "Magisk"
                            null -> stringResource(R.string.module_install_unknown_provider)
                        },
                    )
                    HorizontalDivider()
                    InstallEnvironment(
                        stringResource(R.string.module_install_platform),
                        stringResource(
                            if (state.compatible) R.string.module_install_supported else R.string.module_install_unsupported,
                        ),
                    )
                    HorizontalDivider()
                    InstallEnvironment(
                        stringResource(R.string.module_install_zygisk),
                        stringResource(
                            if (snapshot?.zygiskPresent == true) R.string.module_install_zygisk_detected else R.string.module_install_zygisk_missing,
                        ),
                    )
                }
                Text(
                    stringResource(R.string.module_install_zygisk_hint),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Text(
                    stringResource(R.string.module_install_configuration_hint),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                if (snapshot?.disabled == true) {
                    Text(stringResource(R.string.module_install_disabled_hint), style = MaterialTheme.typography.bodySmall)
                }
            }
        }
        if (state.error != null || state.failureExitCode != null) {
            item {
                val heading = stringResource(
                    if (state.bundledVersion == null) R.string.module_install_bundle_missing else R.string.module_install_failed,
                )
                val detail = state.error ?: stringResource(R.string.module_install_exit_code, state.failureExitCode ?: -1)
                ErrorBanner("$heading: $detail")
            }
        }
        if (state.output.isNotBlank()) {
            item {
                Column {
                    TextButton(onClick = { showOutput = !showOutput }) { Text(stringResource(R.string.module_install_logs)) }
                    AnimatedVisibility(showOutput) {
                        SelectionContainer {
                            Text(
                                state.output,
                                Modifier.fillMaxWidth().padding(12.dp),
                                style = MaterialTheme.typography.mono,
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun InstallValue(label: String, value: String) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        Text(label, Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(
            value,
            style = MaterialTheme.typography.mono.copy(fontSize = MaterialTheme.typography.bodyMedium.fontSize),
            fontWeight = FontWeight.Medium,
        )
    }
}

@Composable
private fun InstallEnvironment(label: String, value: String) {
    Column(
        Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 12.dp),
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        Text(label, style = MaterialTheme.typography.bodyMedium, fontWeight = FontWeight.Medium)
        Text(value, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}
