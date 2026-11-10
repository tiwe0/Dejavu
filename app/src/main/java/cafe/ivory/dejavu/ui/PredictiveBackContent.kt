package cafe.ivory.dejavu.ui

import androidx.activity.BackEventCompat
import androidx.activity.compose.PredictiveBackHandler
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.launch

/** The previous page must be a read-only preview, without live listeners or navigation effects. */
@Composable
internal fun PredictiveBackContent(
    enabled: Boolean,
    canNavigate: Boolean,
    onBack: () -> Unit,
    previousContent: @Composable () -> Unit,
    navigationKey: Any? = Unit,
    content: @Composable () -> Unit,
) {
    val progress = remember { Animatable(0f) }
    val scope = rememberCoroutineScope()
    var settling by remember { mutableStateOf<Job?>(null) }
    var active by remember { mutableStateOf(false) }
    var width by remember { mutableIntStateOf(0) }
    var edge by remember { mutableIntStateOf(BackEventCompat.EDGE_LEFT) }
    var generation by remember { mutableIntStateOf(0) }
    val currentEnabled by rememberUpdatedState(enabled)
    val currentCanNavigate by rememberUpdatedState(canNavigate)
    val currentOnBack by rememberUpdatedState(onBack)
    val currentKey by rememberUpdatedState(navigationKey)

    DisposableEffect(navigationKey, enabled) {
        generation++
        active = false
        settling?.cancel()
        onDispose {
            generation++
            active = false
            settling?.cancel()
        }
    }

    // Always register in the same composition position; disabled leaves root/home back to Android.
    PredictiveBackHandler(enabled = enabled) { events ->
        val route = currentKey
        val request = generation
        val permitted = currentEnabled && currentCanNavigate
        var receivedProgress = false
        var cancelled = false
        var invalidated = false
        settling?.cancelAndJoin()
        settling = null
        active = false
        progress.snapTo(0f)
        fun valid() = permitted && !invalidated && currentEnabled && currentCanNavigate &&
            currentKey == route && generation == request
        try {
            events.collect { event ->
                if (valid()) {
                    receivedProgress = true
                    edge = event.swipeEdge
                    active = true
                    progress.snapTo(event.progress.coerceIn(0f, 1f))
                } else {
                    invalidated = true
                    active = false
                }
            }
            if (valid()) {
                if (receivedProgress) progress.animateTo(1f, tween(140))
                if (valid()) currentOnBack()
            }
        } catch (error: CancellationException) {
            cancelled = true
            if (active && generation == request && currentKey == route) {
                settling = scope.launch {
                    try {
                        progress.animateTo(0f, tween(160))
                    } finally {
                        if (generation == request && currentKey == route) active = false
                    }
                }
            } else {
                active = false
            }
            throw error
        } finally {
            if (!cancelled) {
                active = false
                progress.snapTo(0f)
            }
        }
    }

    val background = MaterialTheme.colorScheme.background
    Box(Modifier.fillMaxSize().onSizeChanged { width = it.width }) {
        if (active) {
            Box(
                Modifier.matchParentSize()
                    .clearAndSetSemantics { }
                    .graphicsLayer {
                        alpha = 0.76f + 0.24f * progress.value
                        scaleX = 0.97f + 0.03f * progress.value
                        scaleY = scaleX
                    }
                    .background(background),
            ) {
                previousContent()
            }
        }
        Box(
            Modifier.fillMaxSize()
                .graphicsLayer {
                    val amount = if (active) progress.value else 0f
                    val direction = if (edge == BackEventCompat.EDGE_LEFT) 1f else -1f
                    translationX = width * 0.24f * amount * direction
                    scaleX = 1f - 0.06f * amount
                    scaleY = scaleX
                    shape = RoundedCornerShape((24f * amount).dp)
                    clip = amount > 0f
                    shadowElevation = 8.dp.toPx() * amount
                }
                .background(background),
        ) {
            content()
        }
        if (active) {
            // A preview must not receive taps while the system owns the back gesture.
            Box(Modifier.matchParentSize().pointerInput(Unit) {
                awaitPointerEventScope {
                    while (true) {
                        awaitPointerEvent(PointerEventPass.Initial).changes.forEach { it.consume() }
                    }
                }
            })
        }
    }
}
