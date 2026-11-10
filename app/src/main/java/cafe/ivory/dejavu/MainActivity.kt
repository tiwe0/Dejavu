package cafe.ivory.dejavu

import android.content.Context
import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
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
import androidx.compose.material.icons.rounded.Translate
import androidx.compose.material.icons.rounded.Settings
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
import androidx.compose.runtime.SideEffect
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.activity.SystemBarStyle
import androidx.compose.ui.graphics.toArgb
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
import cafe.ivory.dejavu.ui.LogsScreen
import cafe.ivory.dejavu.ui.OverviewScreen
import cafe.ivory.dejavu.ui.PluginsScreen
import cafe.ivory.dejavu.ui.RootRequiredScreen
import cafe.ivory.dejavu.ui.ConfigurationScreen
import cafe.ivory.dejavu.ui.PredictiveBackContent
import cafe.ivory.dejavu.ui.theme.DejavuTheme
import kotlinx.coroutines.launch

private enum class ManagerPage(
    @param:StringRes val label: Int,
    @param:StringRes val title: Int,
    val icon: ImageVector,
) {
    Overview(R.string.tab_home, R.string.title_home, Icons.Rounded.Home),
    Plugins(R.string.tab_plugins, R.string.title_plugins, Icons.Rounded.Extension),
    Tools(R.string.tab_configuration, R.string.title_configuration, Icons.Rounded.Settings),
    Logs(R.string.tab_logs, R.string.title_logs, Icons.AutoMirrored.Rounded.Article),
    ;

    val isToolDetail get() = this == Logs
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
            var appearance by remember { mutableStateOf(ManagerAppearanceStore.load(this)) }
            val dark = when (appearance.mode) {
                ManagerThemeMode.SYSTEM -> isSystemInDarkTheme()
                ManagerThemeMode.LIGHT -> false
                ManagerThemeMode.DARK -> true
            }
            DejavuTheme(appearance) {
                val navigationScrim = MaterialTheme.colorScheme.surface.toArgb()
                SideEffect {
                    enableEdgeToEdge(
                        statusBarStyle = SystemBarStyle.auto(android.graphics.Color.TRANSPARENT, android.graphics.Color.TRANSPARENT) { dark },
                        navigationBarStyle = SystemBarStyle.auto(navigationScrim, navigationScrim) { dark },
                    )
                }
                ManagerApp(
                    appearance = appearance,
                    onAppearanceChange = {
                        ManagerAppearanceStore.save(this, it)
                        appearance = it
                    },
                    onLanguageChange = { tag ->
                        if (ManagerLanguage.set(this, tag)) recreate()
                    },
                )
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ManagerApp(
    appearance: ManagerAppearance,
    onAppearanceChange: (ManagerAppearance) -> Unit,
    onLanguageChange: (String?) -> Unit,
    vm: ConsoleViewModel = viewModel(),
) {
    val state by vm.state.collectAsStateWithLifecycle()
    var page by rememberSaveable { mutableStateOf(ManagerPage.Overview) }
    var pluginPageId by remember(page) { mutableStateOf<String?>(null) }
    var scopeEditor by remember(pluginPageId) { mutableStateOf(false) }
    val pluginDetail = pluginPageId?.let { id -> state.directoryPlugins.firstOrNull { it.plugin.manifest.id == id } }
    val nativeDetail = pluginPageId?.let { id -> state.nativeLibrary.firstOrNull { it.artifact.id == id } }
    val pluginsBusy = state.pluginDirectoryLoading || state.pluginsLoading || state.pluginImporting ||
        state.pluginActionId != null || state.nativePluginActionPath != null || state.scopeSavingId != null
    val closePluginPage: () -> Unit = close@ {
        if (pluginsBusy || state.loading) return@close
        if (scopeEditor) scopeEditor = false else {
            vm.closePackageEvents()
            pluginPageId = null
            vm.leavePlugin()
        }
    }
    var showLanguage by remember { mutableStateOf(false) }
    val snackbar = remember { SnackbarHostState() }
    val scope = rememberCoroutineScope()
    val importPluginZip = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) vm.importPluginZip(uri)
    }
    val refreshPlugins = {
        if (state.selectedTarget == null) vm.refreshPluginLibrary() else vm.refreshPlugins()
    }

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


    val lifecycleOwner = LocalLifecycleOwner.current
    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) vm.refresh()
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }

    val renderPage: @Composable (ManagerPage, String?, Boolean, Boolean) -> Unit = { visiblePage, visiblePluginId, visibleScope, preview ->
        when (visiblePage) {
            ManagerPage.Overview -> OverviewScreen(
                state = state,
                onManagePlugins = { page = ManagerPage.Plugins },
                onLinkUnavailable = { vm.message(it, error = true) },
            )
            ManagerPage.Plugins -> PluginsScreen(
                state = state,
                pluginId = visiblePluginId,
                scopeEditor = visibleScope,
                onOpenPluginPage = { id -> if (vm.selectPlugin(id)) pluginPageId = id },
                onOpenScope = { id ->
                    if (vm.selectPlugin(id)) {
                        pluginPageId = id
                        scopeEditor = true
                    }
                },
                onSaveScope = vm::savePluginScope,
                onLoadApps = { vm.loadInstalledApps(force = true) },
                onClosePluginPage = closePluginPage,
                onShowPluginControl = { scopeEditor = false },
                onSetPreferredPort = vm::setPluginListenPort,
                onRefresh = { vm.refreshPlugins() },
                onSelectTarget = { vm.selectTarget(it) },
                onUnload = { target, name -> vm.unloadPlugin(target, name) },
                onLoadLibrary = { target, path -> vm.loadLibraryPlugin(target, path) },
                onImportZip = { importPluginZip.launch(arrayOf("application/zip", "application/x-zip-compressed")) },
                onScanDirectory = refreshPlugins,
                onStartDirectory = { target, id -> vm.startDirectoryPlugin(target, id) },
                onStopDirectory = { target, id -> vm.stopDirectoryPlugin(target, id) },
                onDirectoryEvents = { target, id, port -> vm.openPackageEvents(target, id, port) },
                onCloseDirectoryEvents = { vm.closePackageEvents() },
                onDirectoryAction = { target, id, action -> vm.runDirectoryPluginAction(target, id, action) },
                skillLoader = { target, name -> vm.pluginSkill(target, name) },
                preview = preview,
            )
            ManagerPage.Tools -> ConfigurationScreen(
                appearance = appearance,
                onAppearanceChange = onAppearanceChange,
                onOpenLogs = { page = ManagerPage.Logs },
            )
            ManagerPage.Logs -> LogsScreen(
                state = state,
                onStart = { vm.startLogs() },
                onStop = { vm.stopLogs() },
                onClear = { vm.clearLogs() },
            )
        }
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
                            when {
                                !state.rootGranted -> stringResource(R.string.permission_title)
                                pluginDetail != null -> pluginDetail.plugin.manifest.name
                                nativeDetail != null -> nativeDetail.artifact.name
                                else -> stringResource(page.title)
                            },
                            style = MaterialTheme.typography.titleLarge,
                            fontWeight = FontWeight.SemiBold,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    },
                    navigationIcon = {
                        if (state.rootGranted && (page.isToolDetail || pluginPageId != null)) {
                            IconButton(enabled = !pluginsBusy && !state.loading, onClick = {
                                if (pluginPageId != null) closePluginPage() else page = ManagerPage.Tools
                            }) {
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
            if (!state.rootGranted || page.isToolDetail || pluginPageId != null) return@Scaffold
            Column {
                HorizontalDivider()
                NavigationBar(containerColor = MaterialTheme.colorScheme.surface, tonalElevation = 0.dp) {
                    tabs.forEach { tab ->
                        NavigationBarItem(
                            selected = page == tab,
                            onClick = {
                                vm.leavePlugin()
                                page = tab
                            },
                            enabled = !pluginsBusy && !state.loading,
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
            PredictiveBackContent(
                enabled = page.isToolDetail || pluginPageId != null,
                canNavigate = !pluginsBusy && !state.loading,
                navigationKey = listOf(page, pluginPageId, scopeEditor),
                onBack = {
                    if (pluginPageId != null) closePluginPage() else page = ManagerPage.Tools
                },
                previousContent = {
                    renderPage(
                        if (page.isToolDetail) ManagerPage.Tools else ManagerPage.Plugins,
                        if (scopeEditor) pluginPageId else null,
                        false,
                        true,
                    )
                },
            ) {
                renderPage(page, pluginPageId, scopeEditor, false)
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
