package cafe.ivory.dejavu.data

internal enum class RouterFailure { Discovery, Operation }
internal data class RouterUiState(
    val service: RouterService? = null, val snapshot: RouterSnapshot? = null,
    val loading: Boolean = false, val busy: Boolean = false, val failure: RouterFailure? = null,
) {
    val healthKnown get() = service != null && failure == null
    fun readSucceeded(service: RouterService, snapshot: RouterSnapshot?) =
        copy(service = service, snapshot = snapshot, failure = null)
    // Retain installed/lifecycle information so Stop remains available, but never show stale live data.
    fun readFailed() = copy(snapshot = null, failure = RouterFailure.Discovery)
}
