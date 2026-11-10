package cafe.ivory.dejavu

import android.content.Context
import androidx.annotation.StringRes

enum class ManagerPalette(val key: String, @param:StringRes val labelRes: Int) {
    FOREST("forest", R.string.appearance_palette_forest),
    OCEAN("ocean", R.string.appearance_palette_ocean),
    IRIS("iris", R.string.appearance_palette_iris),
    ROSE("rose", R.string.appearance_palette_rose),
    AMBER("amber", R.string.appearance_palette_amber),
    GRAPHITE("graphite", R.string.appearance_palette_graphite),
    SYSTEM("system", R.string.appearance_palette_system),
}

enum class ManagerThemeMode(val key: String, @param:StringRes val labelRes: Int) {
    SYSTEM("system", R.string.appearance_mode_system),
    LIGHT("light", R.string.appearance_mode_light),
    DARK("dark", R.string.appearance_mode_dark),
}

data class ManagerAppearance(
    val palette: ManagerPalette = ManagerPalette.FOREST,
    val mode: ManagerThemeMode = ManagerThemeMode.SYSTEM,
) {
    companion object {
        internal fun fromStored(palette: String?, mode: String?) = ManagerAppearance(
            palette = ManagerPalette.entries.firstOrNull { it.key == palette } ?: ManagerPalette.FOREST,
            mode = ManagerThemeMode.entries.firstOrNull { it.key == mode } ?: ManagerThemeMode.SYSTEM,
        )
    }
}

object ManagerAppearanceStore {
    private const val PREFERENCES = "manager_appearance"
    private const val PALETTE = "palette"
    private const val MODE = "mode"

    fun load(context: Context): ManagerAppearance {
        val preferences = context.applicationContext.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
        return ManagerAppearance.fromStored(preferences.getString(PALETTE, null), preferences.getString(MODE, null))
    }

    fun save(context: Context, appearance: ManagerAppearance) {
        context.applicationContext.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).edit()
            .putString(PALETTE, appearance.palette.key)
            .putString(MODE, appearance.mode.key)
            .apply()
    }
}
