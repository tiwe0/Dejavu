package cafe.ivory.dejavu.ui

import androidx.compose.animation.AnimatedContentTransitionScope
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.ContentTransform
import androidx.compose.animation.EnterTransition
import androidx.compose.animation.ExitTransition
import androidx.compose.animation.SizeTransform
import androidx.compose.animation.core.FastOutLinearInEasing
import androidx.compose.animation.core.LinearOutSlowInEasing
import androidx.compose.animation.core.MutableTransitionState
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateDpAsState
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
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.ui.unit.Dp

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
    const val ENTER = 320
    const val EXIT = 200
    const val FADE_IN = 220
    const val FADE_OUT = 120
    const val STAGGER = 50

    fun <T> drawer() = spring<T>(dampingRatio = 0.92f, stiffness = 420f)
    fun <T> click() = spring<T>(dampingRatio = 0.55f, stiffness = Spring.StiffnessMedium)
    val drawerOffset = spring(dampingRatio = 0.92f, stiffness = 420f, visibilityThreshold = IntOffset(1, 1))

    /** Push forward into a detail page: new content arrives from the trailing edge. */
    fun forward(): ContentTransform =
        (slideInHorizontally(drawerOffset) { it / 5 } + fadeIn(tween(FADE_IN, easing = LinearOutSlowInEasing)))
            .togetherWith(
                slideOutHorizontally(tween(EXIT, easing = FastOutLinearInEasing)) { -it / 12 } +
                    fadeOut(tween(FADE_OUT, easing = FastOutLinearInEasing)),
            )

    /** Exact reverse of [forward]. */
    fun backward(): ContentTransform =
        (slideInHorizontally(drawerOffset) { -it / 12 } + fadeIn(tween(FADE_IN, easing = LinearOutSlowInEasing)))
            .togetherWith(
                slideOutHorizontally(tween(EXIT, easing = FastOutLinearInEasing)) { it / 5 } +
                    fadeOut(tween(FADE_OUT, easing = FastOutLinearInEasing)),
            )

    /** Sibling destinations (bottom navigation): fade through with a small rise. */
    fun fadeThrough(): ContentTransform =
        (fadeIn(tween(FADE_IN, delayMillis = 60, easing = LinearOutSlowInEasing)) +
            scaleIn(tween(FADE_IN, delayMillis = 60, easing = LinearOutSlowInEasing), initialScale = 0.985f))
            .togetherWith(fadeOut(tween(90, easing = FastOutLinearInEasing)))

    /** Horizontal tab change; [direction] is +1 when moving to a later tab. */
    fun tab(direction: Int): ContentTransform =
        (slideInHorizontally(drawerOffset) { direction * it / 8 } + fadeIn(tween(FADE_IN)))
            .togetherWith(
                slideOutHorizontally(tween(EXIT, easing = FastOutLinearInEasing)) { -direction * it / 16 } +
                    fadeOut(tween(FADE_OUT)),
            )

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

/** Session-scoped memory so first-entry reveals play once, not on every recomposition or tab return. */
internal object RevealMemory {
    private val played = mutableSetOf<String>()
    fun firstTime(key: String): Boolean = played.add(key)
}

/**
 * First-entry reveal: the item rises 12dp and fades in, delayed by its reading order.
 * When [animate] is false the content is shown in place immediately.
 */
@Composable
internal fun Reveal(index: Int, animate: Boolean, content: @Composable () -> Unit) {
    val state = remember { MutableTransitionState(!animate).apply { targetState = true } }
    val delay = (index.coerceAtMost(8)) * Motion.STAGGER
    AnimatedVisibility(
        visibleState = state,
        enter = fadeIn(tween(260, delayMillis = delay, easing = LinearOutSlowInEasing)) +
            slideInVertically(tween(340, delayMillis = delay, easing = LinearOutSlowInEasing)) { it / 8 },
    ) { content() }
}

/**
 * Segmented control whose selection plate slides between options instead of
 * one segment switching off while another switches on.
 */
@Composable
internal fun SlidingSegmented(
    options: List<String>,
    selected: Int,
    onSelect: (Int) -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    height: Dp = 44.dp,
) {
    val colors = MaterialTheme.colorScheme
    val shape = RoundedCornerShape(percent = 50)
    BoxWithConstraints(
        modifier
            .fillMaxWidth()
            .height(height)
            .clip(shape)
            .border(1.dp, colors.outlineVariant, shape)
            .background(colors.surface)
            .padding(3.dp),
    ) {
        val segment = maxWidth / options.size
        val offset by animateDpAsState(segment * selected, Motion.drawer(), label = "segment-offset")
        Box(
            Modifier
                .offset(x = offset)
                .width(segment)
                .fillMaxHeight()
                .clip(shape)
                .background(colors.secondaryContainer),
        )
        Row(Modifier.fillMaxSize().selectableGroup()) {
            options.forEachIndexed { index, label ->
                val active = index == selected
                val weight by animateFloatAsState(if (active) 1f else 0f, tween(Motion.FADE_IN), label = "segment-weight")
                Box(
                    Modifier
                        .weight(1f)
                        .fillMaxHeight()
                        .clip(shape)
                        .selectable(selected = active, enabled = enabled, role = Role.Tab) { onSelect(index) },
                    contentAlignment = Alignment.Center,
                ) {
                    Text(
                        label,
                        style = MaterialTheme.typography.labelLarge,
                        fontWeight = if (active) FontWeight.SemiBold else FontWeight.Medium,
                        color = androidx.compose.ui.graphics.lerp(colors.onSurfaceVariant, colors.onSecondaryContainer, weight)
                            .copy(alpha = if (enabled) 1f else 0.5f),
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.padding(horizontal = 8.dp),
                    )
                }
            }
        }
    }
}
