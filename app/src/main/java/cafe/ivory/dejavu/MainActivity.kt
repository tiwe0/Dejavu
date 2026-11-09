package cafe.ivory.dejavu

import android.content.Context
import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.enableEdgeToEdge
import androidx.annotation.StringRes
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.ArrowBack
import androidx.compose.material.icons.automirrored.rounded.Article
import androidx.compose.material.icons.rounded.Extension
import androidx.compose.material.icons.rounded.Home
import androidx.compose.material.icons.rounded.Refresh
import androidx.compose.material.icons.rounded.Terminal
import androidx.compose.material.icons.rounded.Translate
import androidx.compose.material.icons.rounded.Tune
import androidx.compose.material.icons.rounded.Build
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
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
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
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
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import cafe.ivory.dejavu.data.ConsoleViewModel
import cafe.ivory.dejavu.ui.AppPickerSheet
import cafe.ivory.dejavu.ui.ConsoleScreen
import cafe.ivory.dejavu.ui.LogsScreen
import cafe.ivory.dejavu.ui.OverviewScreen
import cafe.ivory.dejavu.ui.PluginsScreen
import cafe.ivory.dejavu.ui.RootRequiredScreen
import cafe.ivory.dejavu.ui.TargetsScreen
import cafe.ivory.dejavu.ui.ToolsScreen
import cafe.ivory.dejavu.ui.theme.DejavuTheme
import kotlinx.coroutines.launch

private enum class ManagerPage(
    @param:StringRes val label: Int,
    @param:StringRes val title: Int,
    val icon: ImageVector,
) {
    Overview(R.string.tab_home, R.string.title_home, Icons.Rounded.Home),
    Plugins(R.string.tab_plugins, R.string.title_plugins, Icons.Rounded.Extension),
    Scope(R.string.tab_targets, R.string.title_targets, Icons.Rounded.Tune),
    Tools(R.string.tab_tools, R.string.title_tools, Icons.Rounded.Build),
    Logs(R.string.tab_logs, R.string.title_logs, Icons.AutoMirrored.Rounded.Article),
    Lua(R.string.tab_console, R.string.title_console, Icons.Rounded.Terminal),
    ;

    val isToolDetail get() = this == Logs || this == Lua
}

private val tabs = ManagerPage.entries.filterNot { it.isToolDetail }

class MainActivity : ComponentActivity() {
    override fun attachBaseContext(newBase: Context) {
        super.attachBaseContext(ManagerLanguage.localizedContext(newBase))
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            DejavuTheme {
                ManagerApp(onLanguageChange = { tag ->
                    if (ManagerLanguage.set(this, tag)) recreate()
                })
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ManagerApp(
    onLanguageChange: (String?) -> Unit,
    vm: ConsoleViewModel = viewModel(),
) {
    val state by vm.state.collectAsStateWithLifecycle()
    var page by rememberSaveable { mutableStateOf(ManagerPage.Overview) }
    var showLanguage by remember { mutableStateOf(false) }
    var showAppPicker by remember { mutableStateOf(false) }
    val openAppPicker = {
        showAppPicker = true
        vm.loadInstalledApps(force = true)
    }
    val snackbar = remember { SnackbarHostState() }
    val scope = rememberCoroutineScope()
    val importPluginFolder = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if (uri != null) vm.importPluginFolder(uri)
    }
    val refreshPlugins = {
        if (state.selectedTarget == null) vm.refreshPluginLibrary() else vm.refreshPlugins()
    }
    val pluginsBusy = state.pluginDirectoryLoading || state.pluginsLoading || state.pluginImporting ||
        state.pluginActionId != null || state.nativePluginActionPath != null

    LaunchedEffect(state.lastMessage) {
        val message = state.lastMessage ?: return@LaunchedEffect
        scope.launch { snackbar.showSnackbar(message) }
        vm.consumeMessage()
    }

    LaunchedEffect(page, state.selectedTarget, state.rootGranted) {
        if (page != ManagerPage.Plugins || !state.rootGranted) vm.closePackageEvents()
        if (!state.rootGranted) {
            vm.stopLogs()
            return@LaunchedEffect
        }
        if (page == ManagerPage.Logs) vm.startLogs() else vm.stopLogs()
        if (page == ManagerPage.Plugins) refreshPlugins()
    }

    BackHandler(enabled = state.rootGranted && page.isToolDetail) { page = ManagerPage.Tools }

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
                    expandedHeight = 56.dp,
                    colors = TopAppBarDefaults.topAppBarColors(
                        containerColor = MaterialTheme.colorScheme.background,
                    ),
                    title = {
                        Text(
                            stringResource(
                                if (state.rootGranted) page.title
                                else R.string.permission_title,
                            ),
                            style = MaterialTheme.typography.titleLarge,
                            fontWeight = FontWeight.SemiBold,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    },
                    navigationIcon = {
                        if (state.rootGranted && page.isToolDetail) {
                            IconButton(onClick = { page = ManagerPage.Tools }) {
                                Icon(
                                    Icons.AutoMirrored.Rounded.ArrowBack,
                                    contentDescription = stringResource(R.string.navigate_back),
                                )
                            }
                        }
                    },
                    actions = {
                        if (state.rootGranted && !page.isToolDetail) {
                            IconButton(
                                onClick = {
                                    if (page == ManagerPage.Plugins) refreshPlugins() else vm.refresh()
                                },
                                enabled = !state.loading && (page != ManagerPage.Plugins || !pluginsBusy),
                            ) {
                                Icon(
                                    Icons.Rounded.Refresh,
                                    contentDescription = stringResource(R.string.refresh_status),
                                )
                            }
                        }
                        IconButton(onClick = { showLanguage = true }) {
                            Icon(
                                Icons.Rounded.Translate,
                                contentDescription = stringResource(R.string.language),
                            )
                        }
                    },
                )
                if (state.loading) {
                    LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
                }
            }
        },
        bottomBar = {
            if (!state.rootGranted || page.isToolDetail) return@Scaffold
            Column {
                HorizontalDivider()
                NavigationBar(containerColor = MaterialTheme.colorScheme.surface, tonalElevation = 0.dp) {
                    tabs.forEach { tab ->
                        NavigationBarItem(
                            selected = page == tab,
                            onClick = { page = tab },
                            icon = { Icon(tab.icon, contentDescription = null) },
                            label = { Text(stringResource(tab.label)) },
                        )
                    }
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
            when (page) {
                ManagerPage.Overview -> OverviewScreen(
                    state = state,
                    onSelectTarget = { vm.selectTarget(it) },
                    onPickApp = openAppPicker,
                    onManageScope = { page = ManagerPage.Scope },
                    onLinkUnavailable = { vm.message(it, error = true) },
                )
                ManagerPage.Scope -> TargetsScreen(
                    state = state,
                    onPickApp = openAppPicker,
                    onRemove = { vm.removeTarget(it) },
                    onSelect = { vm.selectTarget(it) },
                    onRestart = { vm.restartTarget(it) },
                )
                ManagerPage.Plugins -> PluginsScreen(
                    state = state,
                    onRefresh = { vm.refreshPlugins() },
                    onSelectTarget = { vm.selectTarget(it) },
                    onPickApp = openAppPicker,
                    onUnload = { target, name -> vm.unloadPlugin(target, name) },
                    onLoad = { target, path -> vm.loadPlugin(target, path) },
                    onLoadLibrary = { target, path -> vm.loadLibraryPlugin(target, path) },
                    onImportFolder = { importPluginFolder.launch(null) },
                    onScanDirectory = refreshPlugins,
                    onStartDirectory = { target, id -> vm.startDirectoryPlugin(target, id) },
                    onStopDirectory = { target, id -> vm.stopDirectoryPlugin(target, id) },
                    onDirectoryEvents = { target, id -> vm.openPackageEvents(target, id) },
                    onCloseDirectoryEvents = { vm.closePackageEvents() },
                    skillLoader = { target, name -> vm.pluginSkill(target, name) },
                )
                ManagerPage.Tools -> ToolsScreen(
                    target = state.selectedTarget,
                    onOpenLogs = { page = ManagerPage.Logs },
                    onOpenLua = { page = ManagerPage.Lua },
                )
                ManagerPage.Logs -> LogsScreen(
                    state = state,
                    onStart = { vm.startLogs() },
                    onStop = { vm.stopLogs() },
                    onClear = { vm.clearLogs() },
                )
                ManagerPage.Lua -> ConsoleScreen(
                    state = state,
                    onSelectTarget = { vm.selectTarget(it) },
                    onPickApp = openAppPicker,
                    runner = { vm.runScript(it) },
                )
            }
        }
    }

    if (showLanguage) {
        LanguageDialog(
            onDismiss = { showLanguage = false },
            onSelect = {
                showLanguage = false
                onLanguageChange(it)
            },
        )
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
private fun LanguageDialog(onDismiss: () -> Unit, onSelect: (String?) -> Unit) {
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
                        modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp)
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
