package cafe.ivory.dejavu.data

import android.app.Application
import android.os.Build
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal data class ModuleUiState(
    val loading: Boolean = true,
    val installing: Boolean = false,
    val bundledVersion: String? = null,
    val snapshot: ModuleInstallSnapshot? = null,
    val error: String? = null,
    val failureExitCode: Int? = null,
    val output: String = "",
    val installSucceeded: Boolean = false,
    val compatible: Boolean = Build.VERSION.SDK_INT in 26..35 && "arm64-v8a" in Build.SUPPORTED_ABIS,
) {
    val working get() = installing || snapshot?.installationRunning == true
    val canInstall get() = !loading && !working && error == null && compatible && bundledVersion != null &&
        snapshot?.rootGranted == true && snapshot.provider != null &&
        !snapshot.rebootPending && !snapshot.removalPending
}

/** Installation continues across page changes; no installer output is persisted. */
internal class ModuleViewModel(application: Application) : AndroidViewModel(application) {
    private val installer = ModuleInstaller()
    private var bundle: ModuleBundle? = null
    private var refreshJob: Job? = null
    private var pollingJob: Job? = null
    private val mutableState = MutableStateFlow(ModuleUiState())
    val state = mutableState.asStateFlow()

    fun setVisible(visible: Boolean) {
        if (!visible) {
            pollingJob?.cancel()
            pollingJob = null
        } else if (pollingJob?.isActive != true) {
            pollingJob = viewModelScope.launch {
                while (isActive) {
                    delay(1800)
                    if (mutableState.value.snapshot?.installationRunning == true && !mutableState.value.installing) refresh()
                }
            }
        }
    }

    fun refresh() {
        if (mutableState.value.installing || refreshJob?.isActive == true) return
        mutableState.update { it.copy(loading = true) }
        refreshJob = viewModelScope.launch {
            var payloadError: String? = null
            try {
                val snapshot = withContext(Dispatchers.IO) {
                    if (bundle == null) {
                        try {
                            val assets = getApplication<Application>().assets
                            val metadata = assets.open("dejavu/module.json").bufferedReader().use { it.readText() }
                            val archive = assets.open("dejavu/module.zip").use { it.readBytes() }
                            bundle = ModulePayload.read(archive, metadata)
                        } catch (error: Exception) {
                            payloadError = error.message ?: error.javaClass.simpleName
                        }
                    }
                    installer.inspect()
                }
                mutableState.update {
                    it.copy(snapshot = snapshot, bundledVersion = bundle?.version, error = payloadError,
                        installSucceeded = it.installSucceeded && snapshot.rebootPending)
                }
            } catch (error: CancellationException) {
                throw error
            } catch (error: Exception) {
                mutableState.update { it.copy(error = error.message ?: error.javaClass.simpleName) }
            } finally {
                mutableState.update { it.copy(loading = false) }
            }
        }
    }

    fun install() {
        if (!mutableState.value.canInstall || refreshJob?.isActive == true) return
        val payload = bundle ?: return
        mutableState.update { it.copy(installing = true, installSucceeded = false, error = null, failureExitCode = null, output = "") }
        viewModelScope.launch {
            try {
                val result = withContext(Dispatchers.IO) {
                    installer.install(payload, Build.VERSION.SDK_INT, Build.SUPPORTED_ABIS.toList())
                }
                mutableState.update {
                    it.copy(output = result.output, installSucceeded = result.exitCode == 0,
                        failureExitCode = result.exitCode.takeIf { code -> code != 0 })
                }
                val snapshot = withContext(Dispatchers.IO) { installer.inspect() }
                mutableState.update { it.copy(snapshot = snapshot) }
            } catch (error: CancellationException) {
                throw error
            } catch (error: Exception) {
                mutableState.update { it.copy(error = error.message ?: error.javaClass.simpleName) }
            } finally {
                mutableState.update { it.copy(installing = false) }
            }
        }
    }
}
