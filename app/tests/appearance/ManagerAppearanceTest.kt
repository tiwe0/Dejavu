package cafe.ivory.dejavu

import android.content.Context

fun main() {
    val context = Context()
    check(ManagerAppearanceStore.load(context) == ManagerAppearance())
    check(ManagerPalette.entries.map { it.key }.distinct().size == ManagerPalette.entries.size)
    check(ManagerThemeMode.entries.map { it.key }.distinct().size == ManagerThemeMode.entries.size)
    for (palette in ManagerPalette.entries) {
        for (mode in ManagerThemeMode.entries) {
            val selected = ManagerAppearance(palette, mode)
            ManagerAppearanceStore.save(context, selected)
            check(ManagerAppearanceStore.load(context) == selected)
            check(ManagerAppearanceStore.load(context.recreate()) == selected)
        }
    }
    check(ManagerAppearance.fromStored("unknown", "dark") == ManagerAppearance(ManagerPalette.FOREST, ManagerThemeMode.DARK))
    check(ManagerAppearance.fromStored("ocean", "unknown") == ManagerAppearance(ManagerPalette.OCEAN, ManagerThemeMode.SYSTEM))
    check(ManagerAppearance.fromStored(null, null) == ManagerAppearance())
    val isolated = Context()
    check(ManagerAppearanceStore.load(isolated) == ManagerAppearance())
    println("ManagerAppearance: all palette/mode combinations, recreation, invalid preferences and isolation passed")
}
