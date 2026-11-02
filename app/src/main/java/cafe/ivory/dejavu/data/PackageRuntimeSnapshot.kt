package cafe.ivory.dejavu.data

data class PackageRuntimeSnapshot(val state: DirectoryPluginState, val session: String)

internal fun parsePackageRuntimeSnapshot(raw: String): Map<String, PackageRuntimeSnapshot> {
    val lines = raw.lineSequence().toList()
    require(lines.firstOrNull() == "dejavu-packages-v1") { "Invalid directory plugin status response" }
    val result = linkedMapOf<String, PackageRuntimeSnapshot>()
    for (line in lines.drop(1).filter { it.isNotEmpty() }) {
        val fields = line.split('\t')
        require(fields.size == 3 && fields[0].matches(Regex("[A-Za-z][A-Za-z0-9_-]{0,63}"))) {
            "Invalid directory plugin status row"
        }
        require(fields[2].matches(Regex("[0-9a-f]{16}"))) { "Invalid plugin session" }
        val state = when (fields[1]) {
            "running" -> DirectoryPluginState.RUNNING
            "error" -> DirectoryPluginState.ERROR
            else -> error("Invalid plugin runtime state")
        }
        require(fields[0] !in result) { "Duplicate plugin runtime status" }
        result[fields[0]] = PackageRuntimeSnapshot(state, fields[2])
    }
    return result
}
