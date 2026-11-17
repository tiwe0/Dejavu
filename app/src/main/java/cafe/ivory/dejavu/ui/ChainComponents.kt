package cafe.ivory.dejavu.ui

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.LinearEasing
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.graphics.lerp
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import cafe.ivory.dejavu.ui.theme.mono
import cafe.ivory.dejavu.ui.theme.signal

/** One link of the injection chain: module → root → helper. */
internal data class ChainLink(
    val label: String,
    val detail: String,
    val ok: Boolean,
    val actionLabel: String? = null,
    val onAction: (() -> Unit)? = null,
    val onClick: (() -> Unit)? = null,
)

/**
 * Horizontal chain of status nodes. Healthy links are joined by a solid line; the segment
 * leading into a broken link turns dashed so the break is visible without reading.
 */
@Composable
internal fun InjectionChain(links: List<ChainLink>, modifier: Modifier = Modifier, refreshing: Boolean = false) {
    val colors = MaterialTheme.colorScheme
    val broken = links.firstOrNull { !it.ok }
    // Nodes go dim while status is being re-read, then light up one after another along the chain.
    val reveal = remember { Animatable(links.size.toFloat()) }
    LaunchedEffect(refreshing) {
        if (refreshing) reveal.animateTo(0f, tween(160))
        else reveal.animateTo(links.size.toFloat(), tween(140 * links.size, easing = LinearEasing))
    }
    Surface(
        modifier = modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.large,
        color = colors.surface,
        border = CardDefaults.outlinedCardBorder(),
    ) {
        Column(Modifier.padding(horizontal = 12.dp, vertical = 14.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Box(Modifier.fillMaxWidth().height(14.dp)) {
                val okColor = colors.signal
                val idle = colors.outlineVariant
                val errorColor = colors.error
                Canvas(Modifier.fillMaxWidth().height(14.dp)) {
                    val progress = reveal.value
                    val slot = size.width / links.size
                    val y = size.height / 2
                    for (index in 1 until links.size) {
                        val start = slot * (index - 1) + slot / 2 + 9.dp.toPx()
                        val end = slot * index + slot / 2 - 9.dp.toPx()
                        val intact = links[index - 1].ok && links[index].ok
                        val lit = (progress - index + 0.5f).coerceIn(0f, 1f)
                        drawLine(
                            color = idle,
                            start = Offset(start, y), end = Offset(end, y),
                            strokeWidth = 2.dp.toPx(), cap = StrokeCap.Round,
                            pathEffect = if (intact) null else PathEffect.dashPathEffect(floatArrayOf(6.dp.toPx(), 5.dp.toPx())),
                        )
                        if (intact && lit > 0f) drawLine(
                            color = okColor.copy(alpha = 0.7f),
                            start = Offset(start, y), end = Offset(start + (end - start) * lit, y),
                            strokeWidth = 2.dp.toPx(), cap = StrokeCap.Round,
                        )
                    }
                    links.forEachIndexed { index, link ->
                        val center = Offset(slot * index + slot / 2, y)
                        val lit = (progress - index).coerceIn(0f, 1f)
                        val target = if (link.ok) okColor else errorColor
                        drawCircle(lerp(idle, target, lit), radius = 5.dp.toPx(), center = center)
                        if (link.ok && lit > 0f) drawCircle(okColor.copy(alpha = 0.18f * lit), radius = 7.dp.toPx() * (0.7f + 0.3f * lit), center = center)
                    }
                }
            }
            Row(Modifier.fillMaxWidth()) {
                links.forEach { link ->
                    ChainLabel(link, Modifier.weight(1f))
                }
            }
            androidx.compose.animation.AnimatedVisibility(
                visible = !refreshing && broken?.actionLabel != null && broken.onAction != null,
                enter = androidx.compose.animation.expandVertically(Motion.drawer()) + androidx.compose.animation.fadeIn(tween(Motion.FADE_IN, delayMillis = 80)),
                exit = androidx.compose.animation.shrinkVertically(tween(Motion.EXIT)) + androidx.compose.animation.fadeOut(tween(Motion.FADE_OUT)),
            ) {
                if (broken?.actionLabel != null && broken.onAction != null) Surface(
                    onClick = broken.onAction,
                    modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp),
                    shape = MaterialTheme.shapes.medium,
                    color = colors.errorContainer.copy(alpha = 0.55f),
                    contentColor = colors.onErrorContainer,
                ) {
                    Box(contentAlignment = Alignment.Center) {
                        Text(broken.actionLabel, style = MaterialTheme.typography.labelLarge)
                    }
                }
            }
        }
    }
}

@Composable
private fun ChainLabel(link: ChainLink, modifier: Modifier) {
    val colors = MaterialTheme.colorScheme
    val description = "${link.label}: ${link.detail}"
    val content: @Composable () -> Unit = {
        Column(
            Modifier.padding(horizontal = 2.dp, vertical = 2.dp).clearAndSetSemantics { contentDescription = description },
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            Text(
                link.label,
                style = MaterialTheme.typography.labelLarge,
                color = if (link.ok) colors.onSurface else colors.error,
                maxLines = 1, overflow = TextOverflow.Ellipsis, textAlign = TextAlign.Center,
            )
            Text(
                link.detail,
                style = MaterialTheme.typography.mono,
                color = if (link.ok) colors.onSurfaceVariant else colors.error,
                maxLines = 2, overflow = TextOverflow.Ellipsis, textAlign = TextAlign.Center,
            )
        }
    }
    if (link.onClick != null) {
        Surface(onClick = link.onClick, modifier = modifier, shape = MaterialTheme.shapes.small, color = Color.Transparent) {
            content()
        }
    } else {
        Box(modifier, contentAlignment = Alignment.TopCenter) { content() }
    }
}

/** Small monospace tag such as `LUA · 1.2.0`. */
@Composable
internal fun TypeTag(text: String, modifier: Modifier = Modifier) {
    Text(
        text,
        modifier = modifier
            .clip(MaterialTheme.shapes.extraSmall)
            .background(MaterialTheme.colorScheme.surfaceVariant)
            .padding(horizontal = 6.dp, vertical = 2.dp),
        style = MaterialTheme.typography.mono.copy(fontSize = MaterialTheme.typography.labelSmall.fontSize),
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        maxLines = 1,
    )
}

/** Monogram tile derived from a plugin name; avoids a generic puzzle icon on every row. */
@Composable
internal fun PluginMonogram(name: String, modifier: Modifier = Modifier) {
    val letters = monogram(name)
    Box(
        modifier = modifier
            .size(40.dp)
            .clip(MaterialTheme.shapes.medium)
            .background(MaterialTheme.colorScheme.primaryContainer),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            letters,
            style = MaterialTheme.typography.mono.copy(fontWeight = FontWeight.SemiBold),
            color = MaterialTheme.colorScheme.onPrimaryContainer,
        )
    }
}

internal fun monogram(name: String): String {
    val trimmed = name.trim()
    if (trimmed.isEmpty()) return "?"
    val words = trimmed.split(Regex("[\\s_\\-./]+")).filter { it.isNotEmpty() }
    val first = trimmed.first()
    fun latin(c: Char) = c.code < 0x2E80 && c.isLetterOrDigit()
    return when {
        !latin(first) -> trimmed.take(1)
        words.size >= 2 && latin(words[1].first()) -> "${words[0].first()}${words[1].first()}".uppercase()
        else -> words[0].filter(::latin).take(2).replaceFirstChar { it.uppercase() }.ifEmpty { trimmed.take(1) }
    }
}

/** Live status: a dot plus text. Only the running state uses the signal color and pulses. */
@Composable
internal fun LiveStatus(label: String, live: Boolean, modifier: Modifier = Modifier, error: Boolean = false) {
    val colors = MaterialTheme.colorScheme
    val dotColor by animateColorAsState(
        when {
            error -> colors.error
            live -> colors.signal
            else -> colors.outline
        },
        animationSpec = tween(220),
        label = "status-dot",
    )
    Row(
        modifier = modifier.semantics(mergeDescendants = true) {},
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Box(Modifier.size(12.dp), contentAlignment = Alignment.Center) {
            if (live) {
                val pulse = rememberInfiniteTransition(label = "pulse")
                val scale by pulse.animateFloat(
                    initialValue = 0.6f, targetValue = 1f,
                    animationSpec = infiniteRepeatable(tween(1100), RepeatMode.Reverse),
                    label = "pulse-scale",
                )
                Canvas(Modifier.size(12.dp)) {
                    drawCircle(dotColor.copy(alpha = 0.25f * (1.6f - scale)), radius = size.minDimension / 2 * scale)
                }
            }
            Box(Modifier.size(7.dp).clip(CircleShape).background(dotColor))
        }
        Text(
            label,
            style = MaterialTheme.typography.labelMedium,
            color = when {
                error -> colors.error
                live -> colors.onSurface
                else -> colors.onSurfaceVariant
            },
        )
    }
}
