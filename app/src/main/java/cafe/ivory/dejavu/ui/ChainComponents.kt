package cafe.ivory.dejavu.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkVertically
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
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import cafe.ivory.dejavu.R
import cafe.ivory.dejavu.ui.theme.mono
import cafe.ivory.dejavu.ui.theme.signal

internal enum class ChainNodeState { Healthy, Pending, Broken }

/** One link of the injection chain: module → root → helper. */
internal data class ChainLink(
    val label: String,
    val detail: String,
    val state: ChainNodeState,
    val actionLabel: String? = null,
    val onAction: (() -> Unit)? = null,
    val onClick: (() -> Unit)? = null,
)

/**
 * Horizontal chain of status nodes. Healthy links are joined by a solid line; segments into a
 * pending or broken link turn dashed so the break is visible without reading. Respects RTL.
 */
@Composable
internal fun InjectionChain(
    links: List<ChainLink>,
    modifier: Modifier = Modifier,
    refreshing: Boolean = false,
) {
    if (links.isEmpty()) return
    val colors = MaterialTheme.colorScheme
    val attention = links.firstOrNull { it.state != ChainNodeState.Healthy }
    // Cache the latest actionable link so AnimatedVisibility can smoothly shrink/fade out on recovery.
    var lastActionable by remember { mutableStateOf<ChainLink?>(null) }
    if (attention?.actionLabel != null && attention.onAction != null) {
        lastActionable = attention
    }
    // Nodes go dim while status is being re-read, then light up one after another along the chain.
    val reveal = remember { Animatable(links.size.toFloat()) }
    LaunchedEffect(refreshing, links.size) {
        if (refreshing) reveal.animateTo(0f, tween(160))
        else reveal.animateTo(links.size.toFloat(), tween(140 * links.size, easing = LinearEasing))
    }
    Surface(
        modifier = modifier.fillMaxWidth(),
        shape = MaterialTheme.shapes.large,
        color = colors.surface,
        border = CardDefaults.outlinedCardBorder(),
    ) {
        Column(
            Modifier.padding(horizontal = 12.dp, vertical = 14.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Box(Modifier.fillMaxWidth().height(16.dp)) {
                val okColor = colors.signal
                val pendingColor = colors.tertiary
                val idle = colors.outlineVariant
                val errorColor = colors.error
                Canvas(Modifier.fillMaxWidth().height(16.dp)) {
                    val progress = reveal.value
                    val slot = size.width / links.size
                    val y = size.height / 2
                    val isRtl = layoutDirection == LayoutDirection.Rtl
                    val pad = 9.dp.toPx()
                    fun nodeX(index: Int): Float {
                        val ltrX = slot * index + slot / 2
                        return if (isRtl) size.width - ltrX else ltrX
                    }
                    for (index in 1 until links.size) {
                        val x0 = nodeX(index - 1)
                        val x1 = nodeX(index)
                        val dir = if (x1 >= x0) 1f else -1f
                        val start = x0 + dir * pad
                        val end = x1 - dir * pad
                        if ((end - start) * dir <= 0f) continue
                        val intact = links[index - 1].state == ChainNodeState.Healthy &&
                            links[index].state == ChainNodeState.Healthy
                        val lit = (progress - index + 0.5f).coerceIn(0f, 1f)
                        drawLine(
                            color = idle,
                            start = Offset(start, y),
                            end = Offset(end, y),
                            strokeWidth = 2.dp.toPx(),
                            cap = StrokeCap.Round,
                            pathEffect = if (intact) null else PathEffect.dashPathEffect(floatArrayOf(6.dp.toPx(), 5.dp.toPx())),
                        )
                        if (intact && lit > 0f) {
                            drawLine(
                                color = okColor.copy(alpha = 0.7f),
                                start = Offset(start, y),
                                end = Offset(start + (end - start) * lit, y),
                                strokeWidth = 2.dp.toPx(),
                                cap = StrokeCap.Round,
                            )
                        }
                    }
                    links.forEachIndexed { index, link ->
                        val center = Offset(nodeX(index), y)
                        val lit = (progress - index).coerceIn(0f, 1f)
                        val target = when (link.state) {
                            ChainNodeState.Healthy -> okColor
                            ChainNodeState.Pending -> pendingColor
                            ChainNodeState.Broken -> errorColor
                        }
                        drawCircle(lerp(idle, target, lit), radius = 5.dp.toPx(), center = center)
                        if (link.state == ChainNodeState.Healthy && lit > 0f) {
                            drawCircle(
                                okColor.copy(alpha = 0.18f * lit),
                                radius = 7.dp.toPx() * (0.7f + 0.3f * lit),
                                center = center,
                            )
                        }
                    }
                }
            }
            Row(Modifier.fillMaxWidth()) {
                links.forEach { link ->
                    ChainLabel(link, Modifier.weight(1f))
                }
            }
            val showAction = !refreshing && attention?.actionLabel != null && attention.onAction != null
            AnimatedVisibility(
                visible = showAction,
                enter = expandVertically(Motion.drawer()) + fadeIn(tween(Motion.FADE_IN, delayMillis = 80)),
                exit = shrinkVertically(tween(Motion.EXIT)) + fadeOut(tween(Motion.FADE_OUT)),
            ) {
                val active = lastActionable
                if (active?.actionLabel != null && active.onAction != null) {
                    val isPending = active.state == ChainNodeState.Pending
                    Surface(
                        onClick = active.onAction,
                        modifier = Modifier
                            .fillMaxWidth()
                            .heightIn(min = 48.dp)
                            .semantics { role = Role.Button },
                        shape = MaterialTheme.shapes.large,
                        color = if (isPending) colors.tertiaryContainer else colors.errorContainer,
                        contentColor = if (isPending) colors.onTertiaryContainer else colors.onErrorContainer,
                    ) {
                        Box(
                            Modifier.padding(horizontal = 16.dp, vertical = 10.dp),
                            contentAlignment = Alignment.Center,
                        ) {
                            Text(
                                active.actionLabel,
                                style = MaterialTheme.typography.labelLarge,
                                textAlign = TextAlign.Center,
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun ChainLabel(link: ChainLink, modifier: Modifier) {
    val colors = MaterialTheme.colorScheme
    val stateText = stringResource(
        when (link.state) {
            ChainNodeState.Healthy -> R.string.chain_state_ok
            ChainNodeState.Pending -> R.string.chain_state_pending
            ChainNodeState.Broken -> R.string.chain_state_broken
        },
    )
    val description = "${link.label}, $stateText: ${link.detail}"
    val labelColor = when (link.state) {
        ChainNodeState.Healthy -> colors.onSurface
        ChainNodeState.Pending -> colors.tertiary
        ChainNodeState.Broken -> colors.error
    }
    val detailColor = when (link.state) {
        ChainNodeState.Healthy -> colors.onSurfaceVariant
        ChainNodeState.Pending -> colors.tertiary
        ChainNodeState.Broken -> colors.error
    }
    val content: @Composable () -> Unit = {
        Column(
            Modifier
                .heightIn(min = 48.dp)
                .padding(horizontal = 4.dp, vertical = 4.dp)
                .clearAndSetSemantics { contentDescription = description },
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(2.dp, Alignment.CenterVertically),
        ) {
            Text(
                link.label,
                style = MaterialTheme.typography.labelLarge,
                color = labelColor,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                textAlign = TextAlign.Center,
            )
            Text(
                link.detail,
                style = MaterialTheme.typography.mono,
                color = detailColor,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                textAlign = TextAlign.Center,
            )
        }
    }
    if (link.onClick != null) {
        Surface(
            onClick = link.onClick,
            modifier = modifier.semantics { role = Role.Button },
            shape = MaterialTheme.shapes.small,
            color = Color.Transparent,
        ) {
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
        overflow = TextOverflow.Ellipsis,
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

/** Animated status dot; pauses its infinite pulse when the host lifecycle is backgrounded. */
@Composable
internal fun StatusDot(
    live: Boolean,
    modifier: Modifier = Modifier,
    error: Boolean = false,
) {
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
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    var started by remember(lifecycle) { mutableStateOf(lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)) }
    DisposableEffect(lifecycle) {
        val observer = LifecycleEventObserver { _, _ ->
            started = lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)
        }
        lifecycle.addObserver(observer)
        onDispose { lifecycle.removeObserver(observer) }
    }
    Box(modifier.size(12.dp), contentAlignment = Alignment.Center) {
        if (live && started) {
            val pulse = rememberInfiniteTransition(label = "pulse")
            val scale by pulse.animateFloat(
                initialValue = 0.6f,
                targetValue = 1f,
                animationSpec = infiniteRepeatable(tween(1100), RepeatMode.Reverse),
                label = "pulse-scale",
            )
            Canvas(Modifier.size(12.dp)) {
                drawCircle(dotColor.copy(alpha = 0.25f * (1.6f - scale)), radius = size.minDimension / 2 * scale)
            }
        }
        Box(Modifier.size(7.dp).clip(CircleShape).background(dotColor))
    }
}

/** Live status: a dot plus text. Only the running state uses the signal color and pulses. */
@Composable
internal fun LiveStatus(
    label: String,
    live: Boolean,
    modifier: Modifier = Modifier,
    error: Boolean = false,
) {
    val colors = MaterialTheme.colorScheme
    Row(
        modifier = modifier.semantics(mergeDescendants = true) {},
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(if (label.isNotEmpty()) 6.dp else 0.dp),
    ) {
        StatusDot(live = live, error = error)
        if (label.isNotEmpty()) {
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
}
