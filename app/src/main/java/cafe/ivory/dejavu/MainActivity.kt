package cafe.ivory.dejavu

import android.app.LocaleManager
import android.os.Build
import android.os.Bundle
import android.os.LocaleList
import android.content.Intent
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.annotation.StringRes
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.Article
import androidx.compose.material.icons.rounded.Extension
import androidx.compose.material.icons.rounded.Home
import androidx.compose.material.icons.rounded.Refresh
import androidx.compose.material.icons.rounded.Terminal
import androidx.compose.material.icons.rounded.Translate
import androidx.compose.material.icons.rounded.Tune
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import cafe.ivory.dejavu.data.ConsoleViewModel
import cafe.ivory.dejavu.ui.AppPickerSheet
import cafe.ivory.dejavu.ui.ConsoleScreen
import cafe.ivory.dejavu.ui.HomeScreen
import cafe.ivory.dejavu.ui.LogsScreen
import cafe.ivory.dejavu.ui.PluginsScreen
import cafe.ivory.dejavu.ui.RootRequiredScreen
import cafe.ivory.dejavu.ui.TargetsScreen
import cafe.ivory.dejavu.ui.theme.DejavuTheme
import kotlinx.coroutines.launch

private data class Tab(
    @param:StringRes val label: Int,
    @param:StringRes val title: Int,
    val icon: ImageVector,
)

private val tabs = listOf(
    Tab(R.string.tab_home, R.string.title_home, Icons.Rounded.Home),
    Tab(R.string.tab_targets, R.string.title_targets, Icons.Rounded.Tune),
    Tab(R.string.tab_plugins, R.string.title_plugins, Icons.Rounded.Extension),
    Tab(R.string.tab_logs, R.string.title_logs, Icons.AutoMirrored.Rounded.Article),
    Tab(R.string.tab_console, R.string.title_console, Icons.Rounded.Terminal),
)

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent { DejavuTheme { ConsoleApp() } }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ConsoleApp(vm: ConsoleViewModel = viewModel()) {
    val state by vm.state.collectAsStateWithLifecycle()
    var tabIndex by rememberSaveable { mutableIntStateOf(0) }
    var showLanguage by remember { mutableStateOf(false) }
    var showAppPicker by remember { mutableStateOf(false) }
    val openAppPicker = {
        showAppPicker = true
        vm.loadInstalledApps()
    }
    val snackbar = remember { SnackbarHostState() }
    val scope = rememberCoroutineScope()

    LaunchedEffect(state.lastMessage) {
        val message = state.lastMessage ?: return@LaunchedEffect
        scope.launch { snackbar.showSnackbar(message) }
        vm.consumeMessage()
    }

    LaunchedEffect(tabIndex, state.selectedTarget, state.rootGranted) {
        if (!state.rootGranted) {
            vm.stopLogs()
            return@LaunchedEffect
        }
        if (tabIndex == 3) vm.startLogs() else vm.stopLogs()
        if (tabIndex == 2 && state.selectedTarget != null) vm.refreshPlugins()
    }

    val lifecycleOwner = LocalLifecycleOwner.current
    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) vm.refresh()
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }

    Scaffold(
        modifier = Modifier.fillMaxSize(),
        containerColor = MaterialTheme.colorScheme.background,
        topBar = {
            Column {
                TopAppBar(
                    title = {
                        Text(
                            stringResource(
                                if (state.rootGranted) tabs[tabIndex].title
                                else R.string.permission_title,
                            ),
                            style = MaterialTheme.typography.titleLarge,
                            fontWeight = FontWeight.SemiBold,
                        )
                    },
                    actions = {
                        IconButton(onClick = { showLanguage = true }) {
                            Icon(
                                Icons.Rounded.Translate,
                                contentDescription = stringResource(R.string.language),
                            )
                        }
                        if (state.rootGranted) {
                            IconButton(
                                onClick = { vm.refresh() },
                                enabled = !state.loading,
                            ) {
                                Icon(
                                    Icons.Rounded.Refresh,
                                    contentDescription = stringResource(R.string.refresh_status),
                                )
                            }
                        }
                    },
                )
                if (state.loading) {
                    LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
                }
            }
        },
        bottomBar = {
            if (!state.rootGranted) return@Scaffold
            NavigationBar(containerColor = MaterialTheme.colorScheme.surface) {
                tabs.forEachIndexed { index, tab ->
                    NavigationBarItem(
                        selected = tabIndex == index,
                        onClick = { tabIndex = index },
                        icon = { Icon(tab.icon, contentDescription = stringResource(tab.label)) },
                        label = { Text(stringResource(tab.label)) },
                    )
                }
            }
        },
        snackbarHost = { SnackbarHost(snackbar) },
    ) { padding ->
        Box(
            Modifier
                .fillMaxSize()
                .padding(padding),
        ) {
            if (!state.rootGranted) {
                if (state.loading) {
                    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                        CircularProgressIndicator()
                    }
                } else {
                    val context = LocalContext.current
                    val manager = listOf("me.weishu.kernelsu", "com.topjohnwu.magisk").firstNotNullOfOrNull {
                        context.packageManager.getLaunchIntentForPackage(it)
                    }
                    RootRequiredScreen(
                        state = state,
                        onRetry = { vm.refresh() },
                        onOpenManager = manager?.let {
                            {
                                runCatching {
                                    context.startActivity(
                                        it.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                                    )
                                }
                            }
                        },
                    )
                }
                return@Scaffold
            }
            when (tabIndex) {
                0 -> HomeScreen(
                    state = state,
                    onRefresh = { vm.refresh() },
                    onSelectTarget = { vm.selectTarget(it) },
                    onPickApp = openAppPicker,
                    onNavigate = { tabIndex = it },
                    onRestartTarget = { vm.restartTarget(it) },
                )
                1 -> TargetsScreen(
                    state = state,
                    onPickApp = openAppPicker,
                    onRemove = { vm.removeTarget(it) },
                    onSelect = { vm.selectTarget(it) },
                    onRestart = { vm.restartTarget(it) },
                )
                2 -> PluginsScreen(
                    state = state,
                    onRefresh = { vm.refreshPlugins() },
                    onSelectTarget = { vm.selectTarget(it) },
                    onPickApp = openAppPicker,
                    onUnload = { vm.unloadPlugin(it) },
                    onLoad = { vm.loadPlugin(it) },
                    skillLoader = { vm.pluginSkill(it) },
                )
                3 -> LogsScreen(
                    state = state,
                    onStart = { vm.startLogs() },
                    onStop = { vm.stopLogs() },
                    onClear = { vm.clearLogs() },
                )
                else -> ConsoleScreen(
                    state = state,
                    onSelectTarget = { vm.selectTarget(it) },
                    onPickApp = openAppPicker,
                    runner = { vm.runScript(it) },
                )
            }
        }
    }

    if (showLanguage) {
        LanguageDialog(onDismiss = { showLanguage = false })
    }

    if (showAppPicker) {
        AppPickerSheet(
            state = state,
            onPick = { vm.addTarget(it) },
            onDismiss = { showAppPicker = false },
        )
    }
}

@Composable
private fun LanguageDialog(onDismiss: () -> Unit) {
    val context = LocalContext.current
    val current = if (Build.VERSION.SDK_INT >= 33) {
        context.getSystemService(LocaleManager::class.java)
            .applicationLocales
            .takeIf { !it.isEmpty }
            ?.toLanguageTags()
            .orEmpty()
    } else {
        ""
    }
    val options = listOf(
        null to stringResource(R.string.language_system),
        "en" to stringResource(R.string.language_english),
        "zh-CN" to stringResource(R.string.language_chinese),
    )
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.language_title)) },
        text = {
            Column {
                if (Build.VERSION.SDK_INT < 33) {
                    Text(
                        stringResource(R.string.language_unavailable),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                options.forEach { (tag, label) ->
                    val selected = (tag == null && current.isEmpty()) ||
                        (tag != null && current.startsWith(tag))
                    androidx.compose.foundation.layout.Row(
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        RadioButton(
                            selected = selected,
                            onClick = {
                                if (Build.VERSION.SDK_INT >= 33) {
                                    val manager = context.getSystemService(LocaleManager::class.java)
                                    manager.applicationLocales = if (tag == null) {
                                        LocaleList.getEmptyLocaleList()
                                    } else {
                                        LocaleList.forLanguageTags(tag)
                                    }
                                }
                                onDismiss()
                            },
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
