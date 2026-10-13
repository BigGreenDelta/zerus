package app.zerus.mobile

import org.junit.Assert.*
import org.junit.Test

class UpdateProgressTest {
    @Test fun onlyUnopenedExactConfirmationCanLaunch() {
        assertTrue(UpdateInstallGuard.canOpenConfirmation(7,7,"confirmation",true))
        for(status in listOf("installing","awaiting_result","installed","downloaded"))
            assertFalse(UpdateInstallGuard.canOpenConfirmation(7,7,status,true))
        assertFalse(UpdateInstallGuard.canOpenConfirmation(6,7,"confirmation",true))
        assertFalse(UpdateInstallGuard.canOpenConfirmation(7,7,"confirmation",false))
        assertFalse(UpdateInstallGuard.canOpenConfirmation(-1,-1,"confirmation",true))
    }

    @Test fun committedAndLaunchedSessionsNeverOfferAnotherInstall() {
        for(status in listOf("installing","awaiting_result")) {
            val content=UpdatePresentation.content(UpdateState(status=status,sessionId=7),false,true)
            assertNull(content.primary)
            assertTrue(content.progress)
            assertEquals(UpdateAction.Recover,content.secondary)
            assertEquals(UpdateAction.CancelInstall,content.cancel)
            assertTrue(content.primaryLabel in UpdatePresentation.primaryLabels)
        }
        assertEquals("Waiting for Android…",UpdatePresentation.content(UpdateState(status="awaiting_result",sessionId=7),false,true).primaryLabel)
    }

    @Test fun interruptedConfirmationNeedsExplicitCancellationAndOnlyAvailablePromptCanOpen() {
        val state=UpdateState(status="confirmation",sessionId=7)
        val available=UpdatePresentation.content(state,false,true)
        assertEquals(UpdateAction.Confirm,available.primary)
        assertFalse(available.progress)
        val interrupted=UpdatePresentation.content(state,false,false)
        assertNull(interrupted.primary)
        assertEquals(UpdateAction.CancelInstall,interrupted.cancel)
        assertTrue(interrupted.status.contains("Cancel"))
    }

    @Test fun preparationUsesTheSameActionsAndShowsProgressBeforeSessionCreation() {
        val content=UpdatePresentation.content(UpdateState(status="downloaded"),true,false)
        assertNull(content.primary)
        assertEquals("Preparing installation…",content.primaryLabel)
        assertTrue(content.progress)
        assertTrue(content.status in UpdatePresentation.installationStatuses)
    }
}
