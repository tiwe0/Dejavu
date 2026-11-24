package cafe.ivory.dejavu.ui

import android.content.Context
import android.util.LruCache
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalContext
import androidx.core.graphics.drawable.toBitmap
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/** Lightweight UI-layer cache for app icons so ViewModel state remains pure JVM data. */
internal object AppIconCache {
    private val cache = LruCache<String, ImageBitmap>(128)

    fun get(packageName: String): ImageBitmap? = cache.get(packageName)

    suspend fun load(context: Context, packageName: String): ImageBitmap? {
        cache.get(packageName)?.let { return it }
        return withContext(Dispatchers.IO) {
            runCatching {
                val drawable = context.packageManager.getApplicationIcon(packageName)
                drawable.toBitmap(96, 96).asImageBitmap().also { cache.put(packageName, it) }
            }.getOrNull()
        }
    }
}

@Composable
internal fun rememberAppIcon(packageName: String?): ImageBitmap? {
    val context = LocalContext.current.applicationContext
    val initial = packageName?.let(AppIconCache::get)
    val icon by produceState(initialValue = initial, key1 = packageName) {
        value = if (packageName == null) null else AppIconCache.load(context, packageName)
    }
    return icon
}
