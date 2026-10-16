package app.zerus.mobile

import androidx.compose.animation.core.animate
import androidx.compose.animation.core.tween
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.pulltorefresh.PullToRefreshDefaults
import androidx.compose.material3.pulltorefresh.rememberPullToRefreshState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.nestedscroll.NestedScrollConnection
import androidx.compose.ui.input.nestedscroll.NestedScrollSource
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Velocity
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch

/**
 * Upward travel past the newest message. It mirrors the lists' downward pull:
 * Material3's positional threshold over half the finger travel, and the same
 * bounded content resistance.
 */
class TailPull(private val threshold: Float, private val maximum: Float) {
    /** Finger travel beyond the end of the conversation. */
    var distance by mutableFloatStateOf(0f)
        private set
    val fraction: Float get() = distance * DragMultiplier / threshold
    val armed: Boolean get() = fraction > 1f
    val offset: Float get() = (distance * Resistance).coerceAtMost(maximum)
    /** A pointer is down over the conversation. */
    var held = false
        private set
    fun begin() { held = true }
    /** Upward travel the list could not consume. Returns what the pull took. */
    fun pull(delta: Float): Float {
        if(!held || delta <= 0f) return 0f
        distance += delta
        return delta
    }
    /** Downward travel returns the pull before the list scrolls back. */
    fun unwind(delta: Float): Float {
        if(!held || delta <= 0f || distance <= 0f) return 0f
        val used = delta.coerceAtMost(distance)
        distance -= used
        return used
    }
    /** Lifting the last pointer. True once for travel past the threshold. */
    fun release(): Boolean {
        if(!held) return false
        held = false
        return armed
    }
    fun cancel() { held = false }
    /** The return animation writes the remaining travel; a new touch continues from it. */
    fun settle(value: Float) { distance = value.coerceAtLeast(0f) }
    companion object {
        const val DragMultiplier = .5f
        const val Resistance = .25f
    }
}

/**
 * Pulling up past the newest message is this conversation's explicit refresh,
 * the toolbar's. Flings never pull; automatic and toolbar reads leave the
 * content and the bottom indicator still.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable internal fun ConversationTailRefresh(model: ZerusViewModel, conversation: String,
    modifier: Modifier = Modifier, content: @Composable BoxScope.() -> Unit) {
    if(model.demo || !model.storageReady) { Box(modifier, content = content); return }
    val density = LocalDensity.current
    val threshold = with(density) { PullToRefreshDefaults.PositionalThreshold.toPx() }
    val maximum = with(density) { 28.dp.toPx() }
    val tail = remember(conversation, threshold, maximum) { TailPull(threshold, maximum) }
    val indicator = rememberPullToRefreshState()
    val scope = rememberCoroutineScope()
    var requested by remember(conversation) { mutableStateOf(false) }
    // Only this gesture's read shows the bottom spinner; the top bar shows every read.
    val refreshing = requested && model.detailProgress
    val active by rememberUpdatedState(refreshing)
    var returning by remember(tail) { mutableStateOf<Job?>(null) }
    fun release(refresh: Boolean) {
        if(!tail.held) return
        if(tail.release() && refresh) {
            model.refreshActivity(explicit = true)
            // A refresh that could not start leaves no progress to follow.
            requested = model.detailProgress
        }
        if(!requested) scope.launch { indicator.animateToHidden() }
        returning?.cancel()
        returning = scope.launch { animate(tail.distance, 0f, animationSpec = tween(180)) { value, _ -> tail.settle(value) } }
    }
    LaunchedEffect(tail) { indicator.snapTo(0f) }
    LaunchedEffect(refreshing) {
        if(refreshing) indicator.animateToThreshold()
        else if(requested) { requested = false; indicator.animateToHidden() }
    }
    val connection = remember(tail, indicator) { object : NestedScrollConnection {
        override fun onPreScroll(available: Offset, source: NestedScrollSource): Offset {
            if(source != NestedScrollSource.UserInput || available.y <= 0f) return Offset.Zero
            val used = tail.unwind(available.y)
            if(used > 0f) scope.launch { indicator.snapTo(tail.fraction) }
            return Offset(0f, used)
        }
        override fun onPostScroll(consumed: Offset, available: Offset, source: NestedScrollSource): Offset {
            if(source != NestedScrollSource.UserInput || available.y >= 0f || active) return Offset.Zero
            val used = tail.pull(-available.y)
            if(used > 0f) scope.launch { indicator.snapTo(tail.fraction) }
            return Offset(0f, -used)
        }
        // A pulled list does not fling back past the messages it returns to.
        override suspend fun onPreFling(available: Velocity): Velocity = if(tail.distance > 0f) available else Velocity.Zero
    } }
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    DisposableEffect(lifecycle, tail) {
        val observer = LifecycleEventObserver { _, event -> if(event == Lifecycle.Event.ON_STOP) release(false) }
        lifecycle.addObserver(observer)
        onDispose { lifecycle.removeObserver(observer); tail.cancel() }
    }
    Box(modifier.clipToBounds().nestedScroll(connection).pointerInput(tail) {
        try {
            awaitPointerEventScope {
                while(true) {
                    val event = awaitPointerEvent(PointerEventPass.Initial)
                    if(event.changes.none { it.pressed }) release(true)
                    else if(!tail.held) { returning?.cancel(); tail.begin() }
                }
            }
        } finally { release(false) }
    }) {
        Box(Modifier.fillMaxSize().graphicsLayer { translationY = -tail.offset }, content = content)
        // Material3's top indicator, turned to emerge from the bottom edge.
        PullToRefreshDefaults.Indicator(indicator, refreshing, Modifier.align(Alignment.BottomCenter).rotate(180f))
    }
}
