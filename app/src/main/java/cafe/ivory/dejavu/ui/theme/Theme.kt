package cafe.ivory.dejavu.ui.theme

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
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

private val AppShapes = Shapes(
    extraSmall = androidx.compose.foundation.shape.RoundedCornerShape(4.dp),
    small = androidx.compose.foundation.shape.RoundedCornerShape(8.dp),
    medium = androidx.compose.foundation.shape.RoundedCornerShape(10.dp),
    large = androidx.compose.foundation.shape.RoundedCornerShape(14.dp),
    extraLarge = androidx.compose.foundation.shape.RoundedCornerShape(18.dp),
)

@Composable
fun DejavuTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = if (isSystemInDarkTheme()) DarkColors else LightColors,
        typography = Typography(),
        shapes = AppShapes,
        content = content,
    )
}
