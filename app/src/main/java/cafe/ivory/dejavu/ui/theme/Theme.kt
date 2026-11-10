package cafe.ivory.dejavu.ui.theme

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Typography
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.platform.LocalContext
import cafe.ivory.dejavu.ManagerAppearance
import cafe.ivory.dejavu.ManagerPalette
import cafe.ivory.dejavu.ManagerThemeMode
import androidx.compose.ui.unit.dp

private val DarkColors = darkColorScheme(
    primary = Color(0xFFA0D5B9),
    onPrimary = Color(0xFF102018),
    primaryContainer = Color(0xFF234B36),
    onPrimaryContainer = Color(0xFFD0F1DB),
    secondary = Color(0xFFAAB4C0),
    onSecondary = Color(0xFF15181C),
    secondaryContainer = Color(0xFF2C4838),
    onSecondaryContainer = Color(0xFFD6F2DF),
    tertiary = Color(0xFFC8B98E),
    onTertiary = Color(0xFF201B0F),
    background = Color(0xFF131814),
    onBackground = Color(0xFFE4E5E3),
    surface = Color(0xFF1A211B),
    onSurface = Color(0xFFE4E5E3),
    surfaceVariant = Color(0xFF283329),
    onSurfaceVariant = Color(0xFFBDC8BE),
    surfaceContainerLow = Color(0xFF1A211B),
    surfaceContainer = Color(0xFF222B23),
    surfaceContainerHigh = Color(0xFF2A352C),
    surfaceContainerHighest = Color(0xFF334036),
    outline = Color(0xFF8D9B90),
    outlineVariant = Color(0xFF3E4B41),
    error = Color(0xFFE8A19A),
    onError = Color(0xFF3B0905),
)

private val LightColors = lightColorScheme(
    primary = Color(0xFF14583E),
    onPrimary = Color.White,
    primaryContainer = Color(0xFFD9EDDD),
    onPrimaryContainer = Color(0xFF16442C),
    secondary = Color(0xFF59636D),
    onSecondary = Color.White,
    secondaryContainer = Color(0xFFDDEEDC),
    onSecondaryContainer = Color(0xFF15442D),
    tertiary = Color(0xFF6E5D26),
    onTertiary = Color.White,
    background = Color(0xFFF8F5EE),
    onBackground = Color(0xFF18231D),
    surface = Color(0xFFFCFAF5),
    onSurface = Color(0xFF18231D),
    surfaceVariant = Color(0xFFEDEEE6),
    onSurfaceVariant = Color(0xFF4F5C54),
    surfaceContainerLow = Color(0xFFFBF8F0),
    surfaceContainer = Color(0xFFF3EFE5),
    surfaceContainerHigh = Color(0xFFECE9DE),
    surfaceContainerHighest = Color(0xFFE5E4D8),
    outline = Color(0xFF737D75),
    outlineVariant = Color(0xFFDEDCD3),
    error = Color(0xFF9B332E),
    onError = Color.White,
)

private data class PaletteAccent(
    val light: Color,
    val dark: Color,
    val lightContainer: Color,
    val darkContainer: Color,
    val darkText: Color,
)

/** Static palette colors, also used to preview themes without changing app settings. */
fun managerColorScheme(palette: ManagerPalette, dark: Boolean): ColorScheme {
    if (palette == ManagerPalette.FOREST || palette == ManagerPalette.SYSTEM) {
        return if (dark) DarkColors else LightColors
    }
    val accent = when (palette) {
        ManagerPalette.OCEAN -> PaletteAccent(Color(0xFF205E91), Color(0xFFA0C9F7), Color(0xFFD3E5FF), Color(0xFF164775), Color(0xFF003258))
        ManagerPalette.IRIS -> PaletteAccent(Color(0xFF66549B), Color(0xFFD0BCFF), Color(0xFFEADDFF), Color(0xFF4D3C7F), Color(0xFF352463))
        ManagerPalette.ROSE -> PaletteAccent(Color(0xFF98466A), Color(0xFFFFB0CF), Color(0xFFFFD9E6), Color(0xFF793052), Color(0xFF5B183B))
        ManagerPalette.AMBER -> PaletteAccent(Color(0xFF805618), Color(0xFFF1BF74), Color(0xFFFFDDB0), Color(0xFF63400A), Color(0xFF442B00))
        ManagerPalette.GRAPHITE -> PaletteAccent(Color(0xFF4B5D67), Color(0xFFB7CBD6), Color(0xFFCEE6F2), Color(0xFF344B55), Color(0xFF1D333D))
        else -> error("Palette already handled")
    }
    val primary = if (dark) accent.dark else accent.light
    val primaryContainer = if (dark) accent.darkContainer else accent.lightContainer
    val secondary = lerp(primary, if (dark) Color(0xFFC4C8D0) else Color(0xFF59616D), 0.48f)
    val secondaryContainer = lerp(primaryContainer, if (dark) Color(0xFF30343C) else Color(0xFFE6E9EF), 0.4f)
    val surface = if (dark) lerp(Color(0xFF1B1C1F), accent.dark, 0.035f) else lerp(Color.White, accent.light, 0.015f)
    val background = if (dark) lerp(Color(0xFF121316), accent.dark, 0.025f) else lerp(Color(0xFFFAF9F6), accent.light, 0.018f)
    val onSurface = if (dark) Color(0xFFE5E6E8) else Color(0xFF1B1C1F)
    val onContainer = if (dark) accent.lightContainer else accent.darkText
    val neutralTint = if (dark) accent.dark else accent.light
    fun neutral(value: Long, amount: Float = 0.035f) = lerp(Color(value), neutralTint, amount)
    val base = if (dark) DarkColors else LightColors
    return base.copy(
        primary = primary,
        onPrimary = if (dark) accent.darkText else Color.White,
        primaryContainer = primaryContainer,
        onPrimaryContainer = onContainer,
        inversePrimary = if (dark) accent.light else accent.dark,
        secondary = secondary,
        onSecondary = if (dark) Color(0xFF1B2530) else Color.White,
        secondaryContainer = secondaryContainer,
        onSecondaryContainer = onContainer,
        tertiary = secondary,
        onTertiary = if (dark) Color(0xFF1B2530) else Color.White,
        tertiaryContainer = secondaryContainer,
        onTertiaryContainer = onContainer,
        background = background,
        onBackground = onSurface,
        surface = surface,
        onSurface = onSurface,
        surfaceVariant = if (dark) neutral(0xFF303135) else neutral(0xFFECEDEF),
        onSurfaceVariant = if (dark) Color(0xFFC2C8D0) else Color(0xFF4D5560),
        surfaceTint = primary,
        inverseSurface = if (dark) Color(0xFFE5E6E8) else Color(0xFF303135),
        inverseOnSurface = if (dark) Color(0xFF303135) else Color(0xFFF1F0F2),
        outline = if (dark) Color(0xFF8D969F) else Color(0xFF747B86),
        outlineVariant = if (dark) neutral(0xFF42494F) else neutral(0xFFD5D9E0),
        surfaceBright = if (dark) neutral(0xFF37383D) else neutral(0xFFFEFDFB, 0.01f),
        surfaceDim = if (dark) background else neutral(0xFFDDDFE4),
        surfaceContainerLowest = if (dark) neutral(0xFF0D0E11, 0.01f) else Color.White,
        surfaceContainerLow = if (dark) surface else neutral(0xFFF7F7FA, 0.018f),
        surfaceContainer = if (dark) neutral(0xFF222327) else neutral(0xFFF0F1F5, 0.022f),
        surfaceContainerHigh = if (dark) neutral(0xFF2B2C31) else neutral(0xFFE9EAEF, 0.027f),
        surfaceContainerHighest = if (dark) neutral(0xFF34353B) else neutral(0xFFE2E4E9),
    )
}

/** Resolves wallpaper-derived colors when available; older Android versions use Forest. */
@Composable
fun managerDisplayColorScheme(palette: ManagerPalette, dark: Boolean): ColorScheme {
    if (palette == ManagerPalette.SYSTEM && Build.VERSION.SDK_INT >= 31) {
        val context = LocalContext.current
        return if (dark) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
    }
    return managerColorScheme(palette, dark)
}

private val AppShapes = Shapes(
    extraSmall = androidx.compose.foundation.shape.RoundedCornerShape(4.dp),
    small = androidx.compose.foundation.shape.RoundedCornerShape(8.dp),
    medium = androidx.compose.foundation.shape.RoundedCornerShape(10.dp),
    large = androidx.compose.foundation.shape.RoundedCornerShape(14.dp),
    extraLarge = androidx.compose.foundation.shape.RoundedCornerShape(18.dp),
)

@Composable
fun DejavuTheme(appearance: ManagerAppearance = ManagerAppearance(), content: @Composable () -> Unit) {
    val dark = when (appearance.mode) {
        ManagerThemeMode.SYSTEM -> isSystemInDarkTheme()
        ManagerThemeMode.LIGHT -> false
        ManagerThemeMode.DARK -> true
    }
    MaterialTheme(
        colorScheme = managerDisplayColorScheme(appearance.palette, dark),
        typography = Typography(),
        shapes = AppShapes,
        content = content,
    )
}
