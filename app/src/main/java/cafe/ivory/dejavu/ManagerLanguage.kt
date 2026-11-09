package cafe.ivory.dejavu

import android.app.LocaleManager
import android.content.Context
import android.content.res.Configuration
import android.os.Build
import android.os.LocaleList
import androidx.core.content.edit

internal object ManagerLanguage {
    private const val LANGUAGE_TAG = "language_tag"

    fun current(context: Context): String? =
        if (Build.VERSION.SDK_INT >= 33) {
            val locale = context.getSystemService(LocaleManager::class.java).applicationLocales[0]
            when (locale?.language) {
                "en" -> "en"
                "zh" -> "zh-CN"
                else -> null
            }
        } else {
            preferences(context).getString(LANGUAGE_TAG, null)
        }

    // Returns true when the legacy context needs to be recreated.
    fun set(context: Context, tag: String?): Boolean {
        if (current(context) == tag) return false
        if (Build.VERSION.SDK_INT >= 33) {
            context.getSystemService(LocaleManager::class.java).applicationLocales =
                tag?.let(LocaleList::forLanguageTags) ?: LocaleList.getEmptyLocaleList()
            return false
        }
        preferences(context).edit { putString(LANGUAGE_TAG, tag) }
        return true
    }

    fun localizedContext(context: Context): Context {
        if (Build.VERSION.SDK_INT >= 33) return context
        val tag = current(context) ?: return context
        val config = Configuration().apply { setLocales(LocaleList.forLanguageTags(tag)) }
        return context.createConfigurationContext(config)
    }

    private fun preferences(context: Context) =
        context.getSharedPreferences("manager_language", Context.MODE_PRIVATE)
}
