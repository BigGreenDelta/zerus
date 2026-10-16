package app.zerus.mobile

/** A saved draft is not evidence that Android still owns an external picker. */
internal object AttachmentSelection {
    fun cancel(state: MessageState, selectionId: String): MessageState =
        if (state.pickerId != selectionId || (state.pickerDraft == null && state.pickerId.isBlank())) state
        else state.copy(pickerDraft = null, pickerId = "")

    fun recover(state: MessageState, selectionId: String, launchedId: String, filesBusy: Boolean): MessageState = when {
        filesBusy || state.pickerId != selectionId -> state
        state.pickerDraft != null && selectionId.isNotBlank() && selectionId == launchedId -> state
        else -> cancel(state, selectionId)
    }
}
