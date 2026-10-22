package app.zerus.mobile

/** Reading is adaptive; this policy never retries terminal input. */
data class TerminalPolling(val unchanged: Int = 0) {
    val delayMillis: Long get() = (250L shl unchanged.coerceIn(0,3)).coerceAtMost(2000)
    companion object {
        fun readAllowed(foreground: Boolean,focused: Boolean,selecting: Boolean,reviewing: Boolean,ownedKeyConfirmation: Boolean) =
            foreground && !selecting && !reviewing && (focused || ownedKeyConfirmation)
    }
    fun observed(changed: Boolean) = if(changed) TerminalPolling() else copy(unchanged = (unchanged + 1).coerceAtMost(3))
}

/**
 * Following keeps the native cursor in view. Snapshots have the pane's fixed size, so the
 * followed position depends only on the cursor, the cell size and the viewport.
 */
object TerminalFollow {
    /** The native cursor row, or the last row when the snapshot reports none. */
    fun row(cursorY: Int,rows: Int) = if(cursorY in 0 until rows) cursorY else rows - 1
    /** The bottom while it shows [row]; otherwise [row] at the bottom edge with the rows above it. */
    fun vertical(row: Int,line: Float,viewport: Float,max: Float): Float =
        if(row * line >= max) max.coerceAtLeast(0f) else ((row + 1) * line - viewport).coerceIn(0f,max.coerceAtLeast(0f))
    /** The left edge while the cursor cell fits there; otherwise the cursor cell at the right edge. */
    fun horizontal(cursorX: Int,cols: Int,cell: Float,viewport: Float,max: Float): Float =
        if(cursorX !in 0 until cols) 0f else ((cursorX + 1) * cell - viewport).coerceIn(0f,max.coerceAtLeast(0f))
    /** A settled user scroll resumes following only at the followed position, within half a cell. */
    fun atFollowed(y: Float,x: Float,followedY: Float,followedX: Float,line: Float,cell: Float) =
        kotlin.math.abs(y - followedY) <= line / 2 && kotlin.math.abs(x - followedX) <= cell / 2
}
