package cafe.ivory.dejavu.data

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.json.JSONObject

internal class RouterViewModel(
    private val client: RouterClient = RouterClient(),
    private val coroutineScope: CoroutineScope? = null,
) : ViewModel() {
    private val scope get() = coroutineScope ?: viewModelScope
    private val mutableState = MutableStateFlow(RouterUiState())
    val state = mutableState.asStateFlow()
    private val mutex = Mutex()
    private var polling: Job? = null
    private var visible = false
    private var reading = false

    fun setVisible(value: Boolean) {
        if (visible == value) return
        visible = value
        polling?.cancel()
        // Visibility cancels discovery, while an authorized mutation keeps its response socket.
        if (reading) client.cancelRead()
        if (value) polling = scope.launch {
            var failures = 0
            var ticks = 0
            while (isActive) {
                val discover = ticks == 0 || failures >= 5 || (mutableState.value.service?.running == false && ticks % 5 == 0)
                if (discover) failures = 0
                val success = update(discover = discover)
                ticks++
                failures = if (success) 0 else failures + 1
                delay(3000)
            }
        }
    }
    fun refresh() {
        scope.launch { update(discover = true) }
    }
    private suspend fun update(discover: Boolean): Boolean = mutex.withLock {
        reading = true
        mutableState.update { it.copy(loading = true) }
        try {
            val previous = mutableState.value.service
            if (!discover && previous == null) return@withLock false
            val service = if (discover) withContext(Dispatchers.IO) { client.lifecycle("status") } else previous!!
            if (discover) client.forgetEndpoint()
            val snapshot = if (service.running) withContext(Dispatchers.IO) { client.request("status") } else null
            mutableState.update { it.readSucceeded(service, snapshot) }
            true
        } catch (error: CancellationException) {
            throw error
        } catch (_: Exception) {
            mutableState.update { it.readFailed() }
            false
        } finally {
            reading = false
            mutableState.update { it.copy(loading = false) }
        }
    }
    fun setRunning(running: Boolean) = action {
        val service = client.lifecycle(if (running) "start" else "stop")
        mutableState.update { it.copy(service = service, snapshot = null) }
        if (service.running) client.request("status") else null
    }
    fun reconnect() = command("reconnect")
    fun configure(host: String, port: Int, deviceId: String, token: String?) = command("configure", JSONObject()
        .put("hub_host", host).put("hub_port", port).put("device_id", deviceId).apply {
            if (token != null) put("device_token", token)
        })
    fun putRoute(route: RouterRoute) = command("route_put", JSONObject()
        .put("route", route.route).put("user_id", route.userId).put("package", route.packageName)
        .put("activity", route.activity).put("plugin_path", route.pluginPath))
    fun deleteRoute(route: String) = command("route_delete", JSONObject().put("route", route))
    private fun command(operation: String, arguments: JSONObject = JSONObject()) = action { client.request(operation, arguments) }
    private fun action(block: () -> RouterSnapshot?) {
        if (mutableState.value.busy) return
        mutableState.update { it.copy(busy = true, failure = null) }
        scope.launch {
            mutex.withLock {
                try {
                    val snapshot = withContext(Dispatchers.IO) { block() }
                    mutableState.update { it.copy(snapshot = snapshot, failure = null) }
                } catch (error: CancellationException) {
                    throw error
                } catch (_: Exception) {
                    mutableState.update { it.copy(snapshot = null, failure = RouterFailure.Operation) }
                } finally {
                    mutableState.update { it.copy(busy = false) }
                }
            }
        }
    }
    override fun onCleared() { client.cancelRead() }
}
