package cafe.ivory.dejavu.ui

import android.app.Application
import android.content.Intent
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.EnterExitState
import androidx.compose.animation.core.FastOutLinearInEasing
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.consumeWindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.ArrowBack
import androidx.compose.material.icons.rounded.Extension
import androidx.compose.material.icons.rounded.Home
import androidx.compose.material.icons.rounded.Refresh
import androidx.compose.material.icons.rounded.Settings
import androidx.compose.material.icons.rounded.Translate
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Snackbar
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import cafe.ivory.dejavu.ManagerAppearance
import cafe.ivory.dejavu.ManagerLanguage
import cafe.ivory.dejavu.R
import cafe.ivory.dejavu.data.EnvironmentViewModel
import cafe.ivory.dejavu.data.LogsViewModel
import cafe.ivory.dejavu.data.ManagerRoute
import cafe.ivory.dejavu.data.ManagerViewModelFactories
import cafe.ivory.dejavu.data.ModuleViewModel
import cafe.ivory.dejavu.data.PluginDetailTab
import cafe.ivory.dejavu.data.PluginsViewModel
import cafe.ivory.dejavu.data.RouterViewModel
import cafe.ivory.dejavu.data.operationInProgress
import kotlinx.coroutines.launch

private data class RootNavTab(
    val route: ManagerRoute,
    val labelRes: Int,
    val icon: ImageVector,
)

private val rootTabs = listOf(
    RootNavTab(ManagerRoute.Overview, R.string.tab_overview, Icons.Rounded.Home),
    RootNavTab(ManagerRoute.Plugins, R.string.tab_plugins, Icons.Rounded.Extension),
    RootNavTab(ManagerRoute.Configuration, R.string.tab_configuration, Icons.Rounded.Settings),
)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun ManagerApp(
    appearance: ManagerAppearance,
    onAppearanceChange: (ManagerAppearance) -> Unit,
    onLanguageChange: (String?) -> Unit,
) {
    val context = LocalContext.current
    val application = context.applicationContext as Application
    val envVm: EnvironmentViewModel = viewModel(factory = remember(application) { ManagerViewModelFactories.environment(application) })
    val pluginsVm: PluginsViewModel = viewModel(
        factory = remember(application, envVm) {
            PluginsViewModel.factory(
                application = application,
                scopesProvider = { envVm.state.value.pluginScopes },
                scopeSavingIdProvider = { envVm.state.value.scopeSavingId },
                onMessage = envVm::postMessage,
            )
        },
    )
    val logsVm: LogsViewModel = viewModel(
        factory = remember(application, pluginsVm, envVm) {
            LogsViewModel.factory(
                excludedMarkersProvider = pluginsVm::excludedLogMarkers,
                onError = { detail ->
                    val localized = ManagerLanguage.localizedContext(application)
                    envVm.postMessage(localized.getString(R.string.msg_logcat_failed, detail), error = true)
                },
            )
        },
    )
    val routerVm: RouterViewModel = viewModel()
    val moduleVm: ModuleViewModel = viewModel()

    val envState by envVm.state.collectAsStateWithLifecycle()
    val pluginsState by pluginsVm.state.collectAsStateWithLifecycle()
    val routerState by routerVm.state.collectAsStateWithLifecycle()
    val moduleState by moduleVm.state.collectAsStateWithLifecycle()

    val navigator = rememberManagerNavigator()
    val route = navigator.current
    val pluginsBusy = pluginsState.operationInProgress(envState.scopeSavingId)
    val revealMemory = remember { RevealMemory() }

    var showLanguage by remember { mutableStateOf(false) }
    var scopeDirty by remember { mutableStateOf(false) }
    var pendingLeaveConfirmation by remember { mutableStateOf(false) }
    var pendingLeaveFromGesture by remember { mutableStateOf(false) }
    var snackbarIsError by remember { mutableStateOf(false) }

    val snackbar = remember { SnackbarHostState() }
    val coroutineScope = rememberCoroutineScope()
    val importPluginZip = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) pluginsVm.importPluginZip(uri)
    }

    val performPop: (Boolean) -> Unit = pop@{ fromGesture ->
        if (pluginsBusy) return@pop
        val current = navigator.current
        if (current is ManagerRoute.PluginDetail && current.tab == PluginDetailTab.Scope && scopeDirty) {
            pendingLeaveFromGesture = fromGesture
            pendingLeaveConfirmation = true
            return@pop
        }
        if (current is ManagerRoute.PluginDetail) {
            scopeDirty = false
            pluginsVm.leavePlugin()
        }
        navigator.pop(fromGesture = fromGesture)
    }

    // Deliver one-shot snackbar messages with monotonic IDs so identical consecutive messages are never dropped.
    LaunchedEffect(envState.message?.id) {
        val message = envState.message ?: return@LaunchedEffect
        snackbarIsError = message.isError
        coroutineScope.launch { snackbar.showSnackbar(message.text) }
        envVm.consumeMessage(message.id)
    }

    // Keep plugin targets in sync when scopes refresh.
    LaunchedEffect(envState.pluginScopes) {
        pluginsVm.onScopesUpdated(envState.pluginScopes)
    }

    // Refresh module snapshot once root is granted, and refresh environment when an installation succeeds.
    LaunchedEffect(envState.rootGranted) {
        if (envState.rootGranted) {
            moduleVm.refresh()
        } else {
            pluginsVm.onRootLost()
        }
    }
    LaunchedEffect(moduleState.installSucceeded) {
        if (moduleState.installSucceeded) envVm.refresh()
    }

    // Load plugin library when landing on Overview, Plugins, or restored PluginDetail.
    LaunchedEffect(route.destinationKey, envState.rootGranted) {
        if (route !is ManagerRoute.PluginDetail || !envState.rootGranted) {
            pluginsVm.closePackageEvents()
        }
        if (!envState.rootGranted) return@LaunchedEffect
        when (route) {
            ManagerRoute.Overview -> {
                pluginsVm.refreshPluginLibrary(force = false)
                envVm.loadInstalledApps(force = false)
            }
            ManagerRoute.Plugins -> {
                pluginsVm.refreshPluginLibrary(force = false)
            }
            is ManagerRoute.PluginDetail -> {
                pluginsVm.refreshPluginLibrary(force = false)
                if (pluginsState.activePluginId != route.id) {
                    pluginsVm.selectPlugin(route.id)
                }
            }
            else -> Unit
        }
    }

    // Trigger runtime inspection whenever a plugin detail and its selected target are ready.
    LaunchedEffect(
        route.destinationKey,
        pluginsState.selectedTarget,
        pluginsState.activePluginId,
        pluginsState.directoryPlugins.size,
        pluginsState.nativeLibrary.size,
        envState.rootGranted,
    ) {
        if (route is ManagerRoute.PluginDetail && envState.rootGranted && pluginsState.activePluginId == route.id) {
            pluginsVm.refreshPluginRuntime()
        }
    }

    // Only refresh on genuine foreground resumes, skipping the synchronous ON_RESUME replay on registration.
    val lifecycleOwner = LocalLifecycleOwner.current
    val currentRootGranted by rememberUpdatedState(envState.rootGranted)
    val currentPluginsBusy by rememberUpdatedState(pluginsBusy)
    DisposableEffect(lifecycleOwner) {
        var skipInitialReplay = lifecycleOwner.lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED)
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) {
                if (skipInitialReplay) {
                    skipInitialReplay = false
                    return@LifecycleEventObserver
                }
                if (!currentPluginsBusy) envVm.refresh()
                if (currentRootGranted) moduleVm.refresh()
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }

    // Visibility-bound polling & logcat streaming across foreground/background transitions.
    DisposableEffect(lifecycleOwner, route.destinationKey, envState.rootGranted) {
        fun updateVisibility() {
            val resumed = envState.rootGranted && lifecycleOwner.lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED)
            routerVm.setVisible(route == ManagerRoute.Router && resumed)
            moduleVm.setVisible(route == ManagerRoute.Installer && resumed)
            logsVm.setVisible(route == ManagerRoute.Logs && resumed)
        }
        updateVisibility()
        val observer = LifecycleEventObserver { _, _ -> updateVisibility() }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose {
            lifecycleOwner.lifecycle.removeObserver(observer)
            routerVm.setVisible(false)
            moduleVm.setVisible(false)
            logsVm.setVisible(false)
        }
    }

    val detailActions = remember(navigator, envVm, pluginsVm) {
        PluginDetailActions(
            onSelectTab = { navigator.selectPluginTab(it) },
            onClose = { performPop(false) },
            onSaveScope = { id, targets ->
                pluginsVm.closePackageEvents()
                envVm.savePluginScope(id, targets) { snapshot ->
                    if (snapshot != null) pluginsVm.onScopesUpdated(snapshot.scopes)
                    pluginsVm.refreshPluginRuntime()
                }
            },
            onLoadApps = { envVm.loadInstalledApps(force = false) },
            onSetPreferredPort = pluginsVm::setPluginListenPort,
            onRefreshRuntime = pluginsVm::refreshPluginRuntime,
            onSelectTarget = pluginsVm::selectTarget,
            onUnloadNative = pluginsVm::unloadPlugin,
            onLoadNative = pluginsVm::loadLibraryPlugin,
            onStartDirectory = pluginsVm::startDirectoryPlugin,
            onStopDirectory = pluginsVm::stopDirectoryPlugin,
            onOpenEvents = pluginsVm::openPackageEvents,
            onCloseEvents = pluginsVm::closePackageEvents,
            onRunAction = pluginsVm::runDirectoryPluginAction,
            onLoadSkill = pluginsVm::loadSkill,
            onDismissSkill = pluginsVm::dismissSkill,
        )
    }

    val renderRoute: @Composable (ManagerRoute, Boolean) -> Unit = { visibleRoute, preview ->
        val content: @Composable () -> Unit = {
            when (visibleRoute) {
                ManagerRoute.Overview -> OverviewScreen(
                    envState = envState,
                    moduleState = moduleState,
                    libraryItems = pluginsState.allLibraryItems,
                    onManageModule = { navigator.push(ManagerRoute.Installer) },
                    onManagePlugins = { navigator.selectRoot(ManagerRoute.Plugins) },
                    onRefresh = { envVm.refresh(); moduleVm.refresh() },
                    onOpenPlugin = { id, process ->
                        if (pluginsVm.selectPlugin(id, preferredTarget = process)) {
                            navigator.push(ManagerRoute.PluginDetail(id))
                        }
                    },
                    onRemoveManualTarget = envVm::removeManualTarget,
                    preview = preview,
                )

                ManagerRoute.Plugins -> PluginListScreen(
                    envState = envState,
                    pluginsState = pluginsState,
                    onOpenPlugin = { id ->
                        if (pluginsVm.selectPlugin(id)) {
                            navigator.push(ManagerRoute.PluginDetail(id))
                        }
                    },
                    onImportZip = { importPluginZip.launch(arrayOf("application/zip", "application/x-zip-compressed")) },
                    preview = preview,
                )

                ManagerRoute.Configuration -> ConfigurationScreen(
                    appearance = appearance,
                    onAppearanceChange = onAppearanceChange,
                    onOpenLogs = { navigator.push(ManagerRoute.Logs) },
                    onOpenRouter = { navigator.push(ManagerRoute.Router) },
                    onOpenInstaller = { navigator.push(ManagerRoute.Installer) },
                    onLinkUnavailable = { envVm.postMessage(it, error = true) },
                )

                ManagerRoute.Installer -> ModuleInstallScreen(moduleState, moduleVm::install)

                ManagerRoute.Router -> RouterScreen(
                    state = routerState,
                    onSetRunning = routerVm::setRunning,
                    onReconnect = routerVm::reconnect,
                    onConfigure = routerVm::configureUpstream,
                    onResetUpstream = routerVm::resetUpstream,
                    onReloadPlugins = routerVm::reloadPlugins,
                    onPutRoute = routerVm::putRoute,
                    onDeleteRoute = routerVm::deleteRoute,
                )

                ManagerRoute.Logs -> {
                    val logsState by logsVm.state.collectAsStateWithLifecycle()
                    LogsScreen(
                        state = logsState,
                        onStart = logsVm::start,
                        onStop = logsVm::stop,
                        onClear = logsVm::clear,
                    )
                }

                is ManagerRoute.PluginDetail -> PluginDetailScreen(
                    pluginId = visibleRoute.id,
                    activeTab = if (!preview && route is ManagerRoute.PluginDetail && route.id == visibleRoute.id) route.tab else visibleRoute.tab,
                    envState = envState,
                    pluginsState = pluginsState,
                    actions = detailActions,
                    onScopeDirtyChange = { if (!preview) scopeDirty = it },
                    pendingLeaveConfirmation = !preview && pendingLeaveConfirmation,
                    onResolveLeaveConfirmation = { decision ->
                        pendingLeaveConfirmation = false
                        if (decision != null) {
                            scopeDirty = false
                            pluginsVm.leavePlugin()
                            navigator.pop(fromGesture = pendingLeaveFromGesture)
                        }
                    },
                    preview = preview,
                )
            }
        }
        if (preview) {
            CompositionLocalProvider(LocalRevealMemory provides NoOpRevealMemory) { content() }
        } else {
            content()
        }
    }

    val isLtr = LocalLayoutDirection.current == LayoutDirection.Ltr
    val showRefresh = envState.rootGranted && route != ManagerRoute.Configuration && route != ManagerRoute.Logs

    CompositionLocalProvider(LocalRevealMemory provides revealMemory) {
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
                            val titleText = when {
                                !envState.rootGranted -> stringResource(R.string.permission_title)
                                route == ManagerRoute.Overview -> null
                                route is ManagerRoute.PluginDetail -> pluginsState.findItem(route.id)?.name
                                    ?: stringResource(R.string.title_plugins)
                                route == ManagerRoute.Plugins -> stringResource(R.string.title_plugins)
                                route == ManagerRoute.Configuration -> stringResource(R.string.title_configuration)
                                route == ManagerRoute.Logs -> stringResource(R.string.title_logs)
                                route == ManagerRoute.Router -> stringResource(R.string.router_title)
                                route == ManagerRoute.Installer -> stringResource(R.string.module_install_title)
                                else -> null
                            }
                            AnimatedContent(
                                targetState = titleText,
                                transitionSpec = { Motion.roll() },
                                label = "title",
                            ) { text ->
                                if (text == null) {
                                    Row(verticalAlignment = Alignment.CenterVertically) {
                                        ManagerMark(Modifier.size(40.dp))
                                        Text(
                                            "Dejavu",
                                            style = MaterialTheme.typography.titleLarge,
                                            modifier = Modifier.padding(start = 6.dp),
                                        )
                                    }
                                } else {
                                    Text(
                                        text,
                                        style = MaterialTheme.typography.titleLarge,
                                        fontWeight = FontWeight.SemiBold,
                                        maxLines = 1,
                                        overflow = TextOverflow.Ellipsis,
                                    )
                                }
                            }
                        },
                        navigationIcon = {
                            val backDir = if (isLtr) 1 else -1
                            AnimatedVisibility(
                                visible = envState.rootGranted && navigator.canPop,
                                enter = fadeIn(tween(Motion.FADE_IN)) + slideInHorizontally(Motion.drawerOffset) { backDir * it / 2 },
                                exit = fadeOut(tween(Motion.FADE_OUT)) + slideOutHorizontally(tween(Motion.EXIT)) { backDir * it / 2 },
                            ) {
                                IconButton(
                                    enabled = !pluginsBusy,
                                    onClick = { performPop(false) },
                                ) {
                                    Icon(
                                        Icons.AutoMirrored.Rounded.ArrowBack,
                                        contentDescription = stringResource(R.string.navigate_back),
                                    )
                                }
                            }
                        },
                        actions = {
                            if (showRefresh) {
                                IconButton(
                                    onClick = {
                                        when (route) {
                                            ManagerRoute.Installer -> moduleVm.refresh()
                                            ManagerRoute.Router -> routerVm.refresh()
                                            ManagerRoute.Plugins -> pluginsVm.refreshPluginLibrary(force = true)
                                            is ManagerRoute.PluginDetail -> {
                                                if (route.tab == PluginDetailTab.Scope) envVm.loadInstalledApps(force = true)
                                                else pluginsVm.refreshPluginRuntime()
                                            }
                                            else -> {
                                                envVm.refresh()
                                                moduleVm.refresh()
                                            }
                                        }
                                    },
                                    enabled = when (route) {
                                        ManagerRoute.Installer -> !moduleState.installing && !moduleState.loading
                                        ManagerRoute.Router -> !routerState.busy && !routerState.loading
                                        ManagerRoute.Plugins -> !pluginsBusy && !pluginsState.pluginDirectoryLoading
                                        is ManagerRoute.PluginDetail -> !pluginsBusy && !pluginsState.pluginsLoading
                                        else -> !envState.loading
                                    },
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
                    AnimatedVisibility(
                        visible = envState.loading,
                        enter = fadeIn(tween(Motion.FADE_IN)),
                        exit = fadeOut(tween(Motion.EXIT)),
                    ) {
                        LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
                    }
                }
            },
            bottomBar = {
                AnimatedVisibility(
                    visible = envState.rootGranted && route.isRoot,
                    enter = slideInVertically(Motion.drawerOffset) { it } + fadeIn(tween(Motion.FADE_IN)),
                    exit = slideOutVertically(tween(Motion.EXIT, easing = FastOutLinearInEasing)) { it } +
                        fadeOut(tween(Motion.FADE_OUT)),
                ) {
                    Column {
                        HorizontalDivider()
                        NavigationBar(containerColor = MaterialTheme.colorScheme.surface, tonalElevation = 0.dp) {
                            rootTabs.forEach { tab ->
                                NavigationBarItem(
                                    selected = route == tab.route,
                                    onClick = {
                                        pluginsVm.leavePlugin()
                                        navigator.selectRoot(tab.route)
                                    },
                                    enabled = !pluginsBusy,
                                    icon = { Icon(tab.icon, contentDescription = null) },
                                    label = { Text(stringResource(tab.labelRes)) },
                                )
                            }
                        }
                    }
                }
            },
            snackbarHost = {
                SnackbarHost(snackbar) { data ->
                    Snackbar(
                        snackbarData = data,
                        containerColor = if (snackbarIsError) MaterialTheme.colorScheme.errorContainer
                        else MaterialTheme.colorScheme.inverseSurface,
                        contentColor = if (snackbarIsError) MaterialTheme.colorScheme.onErrorContainer
                        else MaterialTheme.colorScheme.inverseOnSurface,
                    )
                }
            },
        ) { padding ->
            Box(
                Modifier
                    .fillMaxSize()
                    .padding(padding)
                    .consumeWindowInsets(padding),
            ) {
                if (!envState.rootGranted) {
                    if (envState.loading) {
                        Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                            CircularProgressIndicator()
                        }
                    } else {
                        val rootManager = remember(context) { detectRootManager(context) }
                        RootRequiredScreen(
                            loading = envState.loading,
                            onRetry = envVm::refresh,
                            managerLabel = rootManager?.first,
                            onOpenManager = rootManager?.second?.let { intent ->
                                {
                                    runCatching {
                                        context.startActivity(intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
                                    }
                                }
                            },
                        )
                    }
                    return@Scaffold
                }

                // Disable system predictive-back preview when Scope has unsaved edits so the dialog catches Back cleanly.
                val hasUnsavedScope = route is ManagerRoute.PluginDetail && route.tab == PluginDetailTab.Scope && scopeDirty
                var gestureCommitted by remember { mutableStateOf(false) }
                BackHandler(enabled = navigator.canPop && hasUnsavedScope) {
                    performPop(false)
                }
                PredictiveBackContent(
                    enabled = navigator.canPop && !hasUnsavedScope,
                    canNavigate = !pluginsBusy,
                    navigationKey = route.destinationKey,
                    onGestureCommitted = { gestureCommitted = true },
                    onBack = {
                        val fromGesture = gestureCommitted
                        gestureCommitted = false
                        performPop(fromGesture)
                    },
                    previousContent = {
                        navigator.previous?.let { renderRoute(it, true) }
                    },
                ) {
                    AnimatedContent(
                        targetState = route,
                        contentKey = { it.destinationKey },
                        transitionSpec = {
                            when {
                                navigator.consumeSkipPageTransition() -> Motion.none()
                                targetState.depth > initialState.depth -> Motion.forward(isLtr)
                                targetState.depth < initialState.depth -> Motion.backward(isLtr)
                                else -> Motion.fadeThrough()
                            }
                        },
                        label = "page",
                    ) { destination ->
                        val leaving = transition.targetState == EnterExitState.PostExit
                        renderRoute(destination, leaving)
                    }
                }
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

private fun detectRootManager(context: android.content.Context): Pair<String, Intent>? {
    val candidates = listOf(
        "me.bmax.apatch" to "APatch",
        "me.weishu.kernelsu" to "KernelSU",
        "com.topjohnwu.magisk" to "Magisk",
    )
    val pm = context.packageManager
    for ((pkg, fallbackName) in candidates) {
        val intent = pm.getLaunchIntentForPackage(pkg) ?: continue
        val label = runCatching {
            pm.getApplicationLabel(pm.getApplicationInfo(pkg, 0)).toString()
        }.getOrNull()?.takeIf { it.isNotBlank() } ?: fallbackName
        return label to intent
    }
    return null
}
