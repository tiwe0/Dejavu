package cafe.ivory.dejavu.data

import android.app.Application
import android.content.Intent
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import cafe.ivory.dejavu.ManagerLanguage
import cafe.ivory.dejavu.R

internal object ManagerViewModelFactories {
    fun environment(application: Application): ViewModelProvider.Factory = object : ViewModelProvider.Factory {
        @Suppress("UNCHECKED_CAST")
        override fun <T : ViewModel> create(modelClass: Class<T>): T {
            val localized = { ManagerLanguage.localizedContext(application) }
            return EnvironmentViewModel(
                shell = RootShell,
                scopeStore = PluginScopeStore(RootShell),
                loadAppsProvider = { queryLauncherApps(application) },
                strings = EnvironmentStrings(
                    scopeSaved = { localized().getString(R.string.plugin_scope_saved) },
                    scopeWriteError = { detail -> localized().getString(R.string.msg_unable_write_targets) + ": " + detail },
                    manualTargetRemoved = { process -> localized().getString(R.string.manual_target_removed, process) },
                ),
            ).also {
                it.refresh()
                it.loadInstalledApps()
            } as T
        }
    }

    private fun queryLauncherApps(application: Application): List<InstalledApp> {
        val manager = application.packageManager
        val intent = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
        return manager.queryIntentActivities(intent, 0)
            .asSequence()
            .mapNotNull { info ->
                val packageName = info.activityInfo?.packageName ?: return@mapNotNull null
                if (packageName == application.packageName) return@mapNotNull null
                InstalledApp(
                    label = info.loadLabel(manager).toString().ifBlank { packageName },
                    packageName = packageName,
                )
            }
            .distinctBy { it.packageName }
            .sortedBy { it.label.lowercase() }
            .toList()
    }
}
