package cafe.ivory.dejavu.data

const val DEFAULT_PLUGIN_DIRECTORY = "/data/adb/dejavu/plugins"
internal const val MODULE_DIR = "/data/adb/modules/dejavu_zygisk"
internal val SUPPORTED_ANDROID_API_RANGE = 26..37

/** Shared in-process lock covering atomic mutations under [DEFAULT_PLUGIN_DIRECTORY]. */
internal val pluginStorageLock = Any()

/** Generates a POSIX shell predicate ensuring every component of [path] is a real directory (not a symlink). */
internal fun pluginDirectoryGuard(path: String, quote: (String) -> String = RootShell::quote): String {
    val parts = path.split('/').filter { it.isNotEmpty() }
    return parts.indices.joinToString(" && ") { index ->
        val directory = quote("/" + parts.take(index + 1).joinToString("/"))
        "[ -d $directory ] && [ ! -L $directory ]"
    }
}
