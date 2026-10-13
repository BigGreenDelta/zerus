package app.zerus.mobile

enum class UpdateAction { Check, Download, CancelDownload, Install, Confirm, Recover, CancelInstall }
data class UpdateProgressContent(val version:String,val status:String,val primary:UpdateAction?,
    val primaryLabel:String,val secondary:UpdateAction?,val cancel:UpdateAction?,val progress:Boolean)

object UpdatePresentation {
    val primaryLabels=listOf("Check for updates","Download update","Install update","Preparing installation…",
        "Opening confirmation…","Open confirmation","Waiting for Android…","Downloading…","Checking…")

    val installationStatuses=listOf("Saving drafts and preparing installation…","Confirm the update in Android.",
        "The confirmation was interrupted. Cancel this installation to try again.",
        "Waiting for Android to finish installation…","Preparing Android confirmation…","Downloaded and verified. Ready to install.")

    fun content(state:UpdateState,preparing:Boolean,confirmationAvailable:Boolean):UpdateProgressContent {
        val pending=state.sessionId>=0
        val version=(state.downloaded ?: state.available)?.versionName?.let { "Update $it" } ?: "App updates"
        val status=when {
            preparing -> "Saving drafts and preparing installation…"
            pending && state.status=="confirmation" && confirmationAvailable -> "Confirm the update in Android."
            pending && state.status=="confirmation" -> "The confirmation was interrupted. Cancel this installation to try again."
            pending && state.status=="awaiting_result" -> "Waiting for Android to finish installation…"
            pending -> "Preparing Android confirmation…"
            state.status=="downloading" -> "Downloading and verifying the update…"
            state.status=="downloaded" -> "Downloaded and verified. Ready to install."
            state.status=="available" -> "A verified update is available."
            state.status=="checking" -> "Checking for updates…"
            state.status=="loading" -> "Reading saved update status…"
            state.status=="installed" -> "The updated version is installed."
            state.status=="current" -> "This app is up to date."
            state.status=="ahead" -> "This channel has an older version. Wait for a newer build."
            else -> "Check for a newer build."
        }
        val primary=when {
            preparing -> null to "Preparing installation…"
            pending && state.status=="confirmation" && confirmationAvailable -> UpdateAction.Confirm to "Open confirmation"
            pending && state.status=="confirmation" -> null to "Waiting for Android…"
            pending && state.status=="awaiting_result" -> null to "Waiting for Android…"
            pending -> null to "Opening confirmation…"
            state.status=="downloading" -> null to "Downloading…"
            state.status=="checking" || state.status=="loading" -> null to "Checking…"
            state.downloaded!=null -> UpdateAction.Install to "Install update"
            state.available!=null -> UpdateAction.Download to "Download update"
            else -> UpdateAction.Check to "Check for updates"
        }
        return UpdateProgressContent(version,status,primary.first,primary.second,
            if(pending) UpdateAction.Recover else if(primary.first!=UpdateAction.Check) UpdateAction.Check else null,
            if(pending) UpdateAction.CancelInstall else if(state.status=="downloading") UpdateAction.CancelDownload else null,
            preparing || pending && state.status!="confirmation" || state.status in setOf("checking","loading","downloading"))
    }
}
