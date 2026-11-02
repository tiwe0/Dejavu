package cafe.ivory.dejavu.data

internal data class RpcEndpoint(val fileName: String, val pid: Int)

internal fun selectRpcEndpoint(process: String, names: Sequence<String>): RpcEndpoint? {
    val pattern = Regex("${Regex.escape(process)}\\.([0-9]+)\\.rpc")
    return names.mapNotNull { name ->
        val match = pattern.matchEntire(name) ?: return@mapNotNull null
        val pid = match.groupValues[1].toIntOrNull()?.takeIf { it > 0 }
            ?: return@mapNotNull null
        RpcEndpoint(name, pid)
    }.firstOrNull()
}
