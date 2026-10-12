package app.zerus.mobile

/** Installation is an explicit user action and must not cut across an active mutation. */
object UpdateInstallGuard {
    fun reason(state:MessageState,storageReady:Boolean,filesBusy:Boolean,preparing:Boolean,
        flights:Boolean,continuation:Boolean):String = when {
        !storageReady -> "Private storage is not ready. Your drafts must be saved before installing."
        filesBusy || state.pickerDraft != null -> "Finish file selection before installing an update."
        preparing || flights || state.outgoing.any { it.status=="sending" } || state.drafts.any { it.status=="submitting" } ||
            state.sessionActions.any { it.status=="sending" } || state.contextOperations.any { it.status=="sending" } ->
            "Wait for the current message or session command to finish before installing."
        continuation -> "Cancel or finish compact-and-continue before installing."
        else -> ""
    }
    fun ownsCallback(sessionId:Int,nonce:String,resultSessionId:Int,resultNonce:String?) =
        sessionId>=0 && nonce.isNotBlank() && sessionId==resultSessionId && nonce==resultNonce
    fun canStart(sessionId:Int,downloaded:AndroidUpdate?,selected:AndroidUpdate) = sessionId<0 && downloaded==selected
    /** Install already expresses consent to Android's prompt, but only for the session that this tap committed. */
    fun opensConfirmation(requestedSessionId:Int,sessionId:Int,status:String,intentAvailable:Boolean) =
        requestedSessionId>=0 && requestedSessionId==sessionId && status=="confirmation" && intentAvailable
}
