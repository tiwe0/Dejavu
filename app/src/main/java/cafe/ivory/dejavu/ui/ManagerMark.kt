package cafe.ivory.dejavu.ui

import androidx.compose.foundation.Canvas
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.scale
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.graphics.vector.PathParser

/**
 * The in-app mark shares the launcher silhouette, rendered via cached [Path]s on a [Canvas]
 * so theme transitions never rebuild an ImageVector per frame. In light mode a soft badge
 * behind the cream silhouette preserves clear contrast against warm ivory and all palettes.
 */
@Composable
internal fun ManagerMark(modifier: Modifier = Modifier) {
    val colors = MaterialTheme.colorScheme
    val dark = colors.background.luminance() < colors.onBackground.luminance()
    val badge = if (dark) Color.Transparent else colors.primaryContainer.copy(alpha = 0.72f)
    val ink = if (dark) colors.onPrimary else lerp(Color.Black, colors.primary, 0.35f)
    val body = if (dark) lerp(Color(0xFFF8F2E2), colors.primaryContainer, 0.15f) else Color.White
    val outline = if (dark) Color.Transparent else colors.primary.copy(alpha = 0.22f)
    val accent = if (dark) colors.primary else colors.primary
    val paths = remember { MarkPaths() }
    Canvas(modifier = modifier) {
        val factor = size.minDimension / 108f
        if (!dark) {
            drawCircle(color = badge, radius = size.minDimension * 0.44f)
        }
        scale(scaleX = factor, scaleY = factor, pivot = androidx.compose.ui.geometry.Offset.Zero) {
            drawPath(paths.ears, color = ink)
            drawPath(paths.body, color = body)
            if (!dark) drawPath(paths.body, color = outline, style = Stroke(width = 1.6f))
            drawPath(paths.face, color = ink)
            drawPath(paths.accent, color = accent)
        }
    }
}

private class MarkPaths {
    val ears: Path = parse("M39,39v-5h2v-3h2v-3h2v-2h2v-2h2v-2h3v-2h3v-2h7v2h2v5h-2v2h-2v2h-2v2h-2v2h-3v2h-4v2h-5v2zM62,38h7v2h3v2h2v2h2v2h2v2h2v3h3v9h-2v2h-7v-2h-3v-2h-2v-3h-3z")
    val body: Path = parse("M49,35h9v2h3v2h2v5h2v5h2v12h-2v4h-3v5h2v2h3v2h2v3h2v4h2v7H29v-7h2v-4h2v-4h2v-3h3v-4h-2v-3h-2V49h2v-5h2v-3h2v-2h4v-2h5z")
    val face: Path = parse("M42,48h2v3h-2v2h-2v-3h2zM58,48h2v2h2v2h1v2h-2v-2h-2v-2h-1zM48,57h6v2h2v2h1v5h-2v2h-8v-2h-2v-5h1v-2h2z")
    val accent: Path = parse("M73,26.5l3.8,3.8 -3.8,3.8 -3.8,-3.8z")

    private fun parse(data: String): Path = PathParser().parsePathString(data).toPath()
}
