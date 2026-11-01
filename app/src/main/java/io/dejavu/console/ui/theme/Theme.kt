package io.dejavu.console.ui.theme

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
    primary = Color(0xFF8FC7A8),
    onPrimary = Color(0xFF102018),
    primaryContainer = Color(0xFF24352C),
    onPrimaryContainer = Color(0xFFD5EBDD),
    secondary = Color(0xFFAAB4C0),
    onSecondary = Color(0xFF15181C),
    secondaryContainer = Color(0xFF292D33),
    onSecondaryContainer = Color(0xFFDCE2E8),
    tertiary = Color(0xFFC8B98E),
    onTertiary = Color(0xFF201B0F),
    background = Color(0xFF101112),
    onBackground = Color(0xFFE4E5E3),
    surface = Color(0xFF17181A),
    onSurface = Color(0xFFE4E5E3),
    surfaceVariant = Color(0xFF202225),
    onSurfaceVariant = Color(0xFFAEB3B9),
    outline = Color(0xFF3B3E43),
    outlineVariant = Color(0xFF2A2D31),
    error = Color(0xFFE8A19A),
    onError = Color(0xFF3B0905),
)

private val LightColors = lightColorScheme(
    primary = Color(0xFF2F6B4F),
    onPrimary = Color.White,
    primaryContainer = Color(0xFFD8EADF),
    onPrimaryContainer = Color(0xFF123526),
    secondary = Color(0xFF59636D),
    onSecondary = Color.White,
    secondaryContainer = Color(0xFFE7EBEE),
    onSecondaryContainer = Color(0xFF1B2227),
    tertiary = Color(0xFF6E5D26),
    onTertiary = Color.White,
    background = Color(0xFFF6F6F4),
    onBackground = Color(0xFF1A1B1B),
    surface = Color(0xFFFCFCFA),
    onSurface = Color(0xFF1A1B1B),
    surfaceVariant = Color(0xFFECEDEA),
    onSurfaceVariant = Color(0xFF555A5E),
    outline = Color(0xFF8A8F92),
    outlineVariant = Color(0xFFD7D9D5),
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
