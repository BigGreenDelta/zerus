package app.zerus.mobile

import android.view.ViewTreeObserver
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalView
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner

internal data class GitViewportItem(val key: Any?, val offset: Int, val size: Int)

/** A placed item must intersect the actual list viewport; precomposition alone proves nothing. */
internal object GitStatusViewport {
    fun keys(items: List<GitViewportItem>, start: Int, end: Int): Set<String> {
        if (end <= start) return emptySet()
        val result = linkedSetOf<String>()
        for (item in items) {
            val key = item.key as? String ?: continue
            if (!key.startsWith("session:") || key.length == "session:".length || item.size <= 0) continue
            if (item.offset.toLong() >= end.toLong() || item.offset.toLong() + item.size.toLong() <= start.toLong()) continue
            result += key
            if (result.size == GitStatusController.QUEUE_LIMIT) break
        }
        return result
    }
}

/** Own this once per session list, independently of the app's catalog polling and onStop. */
@Composable internal fun rememberGitViewport(controller: GitStatusController, listState: LazyListState, enabled: Boolean): Set<String> {
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    val view = LocalView.current
    DisposableEffect(controller, lifecycle, view, enabled) {
        val tree = view.viewTreeObserver
        var focus = view.hasWindowFocus()
        var listening = false
        var active = true
        fun update() { if (active) controller.setForeground(enabled && listening && lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED) && focus) }
        val focusListener = ViewTreeObserver.OnWindowFocusChangeListener { value ->
            // Use the event itself: even loss and regain within one frame invalidate origins.
            focus = value
            update()
        }
        val lifecycleListener = LifecycleEventObserver { _, _ -> update() }
        if (tree.isAlive) { tree.addOnWindowFocusChangeListener(focusListener); listening = true }
        lifecycle.addObserver(lifecycleListener)
        update()
        onDispose {
            active = false
            listening = false
            lifecycle.removeObserver(lifecycleListener)
            if (tree.isAlive) tree.removeOnWindowFocusChangeListener(focusListener)
            controller.setForeground(false)
        }
    }
    val keys by remember(listState) { derivedStateOf {
        val layout = listState.layoutInfo
        GitStatusViewport.keys(layout.visibleItemsInfo.map { GitViewportItem(it.key, it.offset, it.size) },
            layout.viewportStartOffset, layout.viewportEndOffset)
    } }
    return keys
}

/** Candidate values survive identical catalog refreshes; only a visible card owns a lease. */
@Composable internal fun rememberGitBadge(controller: GitStatusController, candidate: GitStatusCandidate?, visible: Boolean): GitStatusBadge? {
    DisposableEffect(controller, candidate, visible) {
        val lease = if (visible && candidate != null) controller.watch(candidate, true) else null
        onDispose { lease?.close() }
    }
    // Read revision to observe expiry/completion, then calculate on every composition. A remembered
    // badge would hide the current access Snapshot reads from changes in connection/capability.
    return controller.revision.collectAsState().value.let { candidate?.let(controller::badge) }
}
