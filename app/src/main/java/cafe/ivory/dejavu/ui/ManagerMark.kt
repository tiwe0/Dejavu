package cafe.ivory.dejavu.ui

import androidx.compose.foundation.Image
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.PathParser
import androidx.compose.ui.graphics.vector.rememberVectorPainter
import androidx.compose.ui.unit.dp

/** The in-app mark shares the launcher silhouette, with fills from the current theme. */
@Composable
internal fun ManagerMark(modifier: Modifier = Modifier) {
    val colors = MaterialTheme.colorScheme
    val dark = colors.background.luminance() < colors.onBackground.luminance()
    val ink = if (dark) colors.onPrimary else lerp(Color.Black, colors.primary, 0.4f)
    val body = lerp(Color(0xFFF8F2E2), colors.primaryContainer, 0.15f)
    val accent = if (dark) colors.primary else colors.primaryContainer
    val vector = remember(ink, body, accent) {
        ImageVector.Builder(
            name = "DejavuManagerMark",
            defaultWidth = 108.dp,
            defaultHeight = 108.dp,
            viewportWidth = 108f,
            viewportHeight = 108f,
        ).apply {
            addPath(pathData = EARS, fill = SolidColor(ink))
            addPath(pathData = BODY, fill = SolidColor(body))
            addPath(pathData = FACE, fill = SolidColor(ink))
            addPath(pathData = ACCENT, fill = SolidColor(accent))
        }.build()
    }
    Image(rememberVectorPainter(vector), contentDescription = null, modifier = modifier)
}

private fun nodes(path: String) = PathParser().parsePathString(path).toNodes()

// Geometry matches ic_launcher_foreground.xml; the launcher asset remains unchanged.
private val EARS = nodes("M39,39v-5h2v-3h2v-3h2v-2h2v-2h2v-2h3v-2h3v-2h7v2h2v5h-2v2h-2v2h-2v2h-2v2h-3v2h-4v2h-5v2zM62,38h7v2h3v2h2v2h2v2h2v2h2v3h3v9h-2v2h-7v-2h-3v-2h-2v-3h-3z")
private val BODY = nodes("M49,35h9v2h3v2h2v5h2v5h2v12h-2v4h-3v5h2v2h3v2h2v3h2v4h2v7H29v-7h2v-4h2v-4h2v-3h3v-4h-2v-3h-2V49h2v-5h2v-3h2v-2h4v-2h5z")
private val FACE = nodes("M42,48h2v3h-2v2h-2v-3h2zM58,48h2v2h2v2h1v2h-2v-2h-2v-2h-1zM48,57h6v2h2v2h1v5h-2v2h-8v-2h-2v-5h1v-2h2z")
private val ACCENT = nodes("M73,26.5l3.8,3.8 -3.8,3.8 -3.8,-3.8z")
