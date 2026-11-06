package cafe.ivory.dejavu.data

internal data class RpcEndpoint(val fileName: String, val pid: Int)

internal fun selectRpcEndpoint(process: String, names: Sequence<String>, livePids: Set<Int>): RpcEndpoint? {
    val pattern = Regex("${Regex.escape(process)}\\.([0-9]+)\\.rpc")
    return names.mapNotNull { name ->
        val match = pattern.matchEntire(name) ?: return@mapNotNull null
        val pid = match.groupValues[1].toIntOrNull()?.takeIf { it > 0 }
            ?: return@mapNotNull null
        if (pid !in livePids) return@mapNotNull null
        RpcEndpoint(name, pid)
    }.firstOrNull()
}

internal fun decodeRpcToken(value: String): ByteArray? {
    if (!value.matches(Regex("[0-9a-fA-F]{64}"))) return null
    val bytes = value.chunked(2).map { it.toInt(16).toByte() }.toByteArray()
    return bytes.takeIf { token -> token.any { it != 0.toByte() } }
}

internal fun validRpcSocketName(value: String): Boolean = value.matches(Regex("[A-Za-z0-9_.-]{1,63}"))
