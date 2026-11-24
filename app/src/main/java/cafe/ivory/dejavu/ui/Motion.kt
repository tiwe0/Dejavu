package cafe.ivory.dejavu.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.ContentTransform
import androidx.compose.animation.EnterTransition
import androidx.compose.animation.ExitTransition
import androidx.compose.animation.SizeTransform
import androidx.compose.animation.core.FastOutLinearInEasing
import androidx.compose.animation.core.LinearOutSlowInEasing
import androidx.compose.animation.core.MutableTransitionState
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleIn
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.slideOutVertically
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import kotlin.math.roundToInt

/**
 * One motion vocabulary for the Manager.
 *
 * - Drawer: damped slide for pages, tabs and the selection indicator (large surfaces,
 *   no visible overshoot).
 * - Click: short, slightly springy press feedback for primary actions.
 * - Exits run at roughly two thirds of the matching enter duration.
 *
 * Compose scales every spec by the system animator duration setting, so "remove
 * animations" in accessibility settings makes all of these land instantly.
 */
internal object Motion {
    const val EXIT = 200
    const val FADE_IN = 220
    const val FADE_OUT = 120
    const val STAGGER = 50
    const val MAX_REVEAL_ITEMS = 8

    fun <T> drawer() = spring<T>(dampingRatio = 0.92f, stiffness = 420f)
    fun <T> click() = spring<T>(dampingRatio = 0.55f, stiffness = Spring.StiffnessMedium)
    val drawerOffset = spring(dampingRatio = 0.92f, stiffness = 420f, visibilityThreshold = IntOffset(1, 1))

    /** Push forward into a detail page: new content arrives from the trailing edge. */
    fun forward(ltr: Boolean = true): ContentTransform {
        val sign = if (ltr) 1 else -1
        return (slideInHorizontally(drawerOffset) { sign * it / 5 } + fadeIn(tween(FADE_IN, easing = LinearOutSlowInEasing)))
            .togetherWith(
                slideOutHorizontally(tween(EXIT, easing = FastOutLinearInEasing)) { -sign * it / 12 } +
                    fadeOut(tween(FADE_OUT, easing = FastOutLinearInEasing)),
            )
    }

    /** Exact reverse of [forward]. */
    fun backward(ltr: Boolean = true): ContentTransform {
        val sign = if (ltr) 1 else -1
        return (slideInHorizontally(drawerOffset) { -sign * it / 12 } + fadeIn(tween(FADE_IN, easing = LinearOutSlowInEasing)))
            .togetherWith(
                slideOutHorizontally(tween(EXIT, easing = FastOutLinearInEasing)) { sign * it / 5 } +
                    fadeOut(tween(FADE_OUT, easing = FastOutLinearInEasing)),
            )
    }

    /** Sibling destinations (bottom navigation): fade through with a small rise. */
    fun fadeThrough(): ContentTransform =
        (fadeIn(tween(FADE_IN, delayMillis = 60, easing = LinearOutSlowInEasing)) +
            scaleIn(tween(FADE_IN, delayMillis = 60, easing = LinearOutSlowInEasing), initialScale = 0.985f))
            .togetherWith(fadeOut(tween(90, easing = FastOutLinearInEasing)))

    /** Horizontal tab change; [direction] is +1 when moving to a later tab. */
    fun tab(direction: Int, ltr: Boolean = true): ContentTransform {
        val sign = if (ltr) direction else -direction
        return (slideInHorizontally(drawerOffset) { sign * it / 8 } + fadeIn(tween(FADE_IN)))
            .togetherWith(
                slideOutHorizontally(tween(EXIT, easing = FastOutLinearInEasing)) { -sign * it / 16 } +
                    fadeOut(tween(FADE_OUT)),
            )
    }

    /** A value replaced by the next one rolls upward (state labels, button text). */
    fun roll(): ContentTransform = ContentTransform(
        targetContentEnter = slideInVertically(tween(FADE_IN, easing = LinearOutSlowInEasing)) { it / 2 } + fadeIn(tween(FADE_IN)),
        initialContentExit = slideOutVertically(tween(FADE_OUT, easing = FastOutLinearInEasing)) { -it / 2 } + fadeOut(tween(FADE_OUT)),
        sizeTransform = SizeTransform(clip = false),
    )

    fun none(): ContentTransform = EnterTransition.None togetherWith ExitTransition.None
}

/** Scale-on-press for primary actions; pass the same [source] to the clickable component. */
@Composable
internal fun Modifier.pressScale(source: MutableInteractionSource, pressed: Float = 0.97f): Modifier {
    val isPressed by source.collectIsPressedAsState()
    val scale by animateFloatAsState(if (isPressed) pressed else 1f, Motion.click(), label = "press")
    return graphicsLayer { scaleX = scale; scaleY = scale }
}

/** Scoped memory so first-entry reveals play once per session and never pollute previews or tests. */
internal open class RevealMemory {
    private val played = mutableSetOf<String>()
    open fun consumeFirstTime(key: String): Boolean = played.add(key)
}

internal object NoOpRevealMemory : RevealMemory() {
    override fun consumeFirstTime(key: String): Boolean = false
}

internal val LocalRevealMemory = compositionLocalOf { RevealMemory() }

/**
 * First-entry reveal for above-the-fold items only (`index < MAX_REVEAL_ITEMS`).
 * Items beyond the first viewport or after scrolling land immediately with zero delay.
 */
@Composable
internal fun Reveal(index: Int, animate: Boolean, content: @Composable () -> Unit) {
    if (!animate || index >= Motion.MAX_REVEAL_ITEMS) {
        content()
        return
    }
    val state = remember { MutableTransitionState(false).apply { targetState = true } }
    val delay = index * Motion.STAGGER
    AnimatedVisibility(
        visibleState = state,
        enter = fadeIn(tween(260, delayMillis = delay, easing = LinearOutSlowInEasing)) +
            slideInVertically(tween(340, delayMillis = delay, easing = LinearOutSlowInEasing)) { it / 8 },
    ) { content() }
}

/**
 * Segmented control whose selection plate slides smoothly between options, respects RTL,
 * guarantees >= 48dp touch targets and grows vertically under large font scales.
 */
@Composable
internal fun SlidingSegmented(
    options: List<String>,
    selected: Int,
    onSelect: (Int) -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
) {
    if (options.isEmpty()) return
    val safeSelected = selected.coerceIn(options.indices)
    val colors = MaterialTheme.colorScheme
    val shape = RoundedCornerShape(percent = 50)
    var innerWidthPx by remember { mutableIntStateOf(0) }
    val animatedIndex by animateFloatAsState(safeSelected.toFloat(), Motion.drawer(), label = "segment-index")
    Box(
        modifier
            .fillMaxWidth()
            .heightIn(min = 48.dp)
            .clip(shape)
            .border(1.dp, colors.outlineVariant, shape)
            .background(colors.surface)
            .padding(3.dp),
    ) {
        Box(
            Modifier
                .fillMaxWidth()
                .height(IntrinsicSize.Min)
                .onSizeChanged { innerWidthPx = it.width },
        ) {
            Box(
                Modifier
                    .offset { IntOffset(((innerWidthPx.toFloat() / options.size) * animatedIndex).roundToInt(), 0) }
                    .fillMaxWidth(1f / options.size)
                    .fillMaxHeight()
                    .clip(shape)
                    .background(colors.secondaryContainer),
            )
            Row(
                Modifier
                    .fillMaxWidth()
                    .selectableGroup(),
            ) {
                options.forEachIndexed { index, label ->
                    val active = index == safeSelected
                    val weight by animateFloatAsState(if (active) 1f else 0f, tween(Motion.FADE_IN), label = "segment-weight")
                    Box(
                        Modifier
                            .weight(1f)
                            .heightIn(min = 42.dp)
                            .clip(shape)
                            .selectable(selected = active, enabled = enabled, role = Role.Tab) { onSelect(index) }
                            .padding(horizontal = 8.dp, vertical = 10.dp),
                        contentAlignment = Alignment.Center,
                    ) {
                        Text(
                            label,
                            style = MaterialTheme.typography.labelLarge,
                            fontWeight = if (active) FontWeight.SemiBold else FontWeight.Medium,
                            color = lerp(colors.onSurfaceVariant, colors.onSecondaryContainer, weight)
                                .copy(alpha = if (enabled) 1f else 0.5f),
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                }
            }
        }
    }
}
