package app.zerus.mobile

import org.junit.Assert.*
import org.junit.Test

class CatalogRefreshStateTest {
    private val workspaces=listOf("one" to "First workspace","two" to "Second workspace")

    @Test fun actualStagesRemainVisibleUntilPublicationAndCompletionStaysReadable() {
        var state=CatalogRefreshState().begin(1,workspaces)
        assertTrue(state.running);assertTrue(state.summary.contains("Syncing"))
        state=state.stage(1,"one",CatalogRefreshStage.Capabilities)
        assertTrue(state.summary.contains("Checking workspace"))
        state=state.stage(1,"one",CatalogRefreshStage.Reports)
        assertTrue(state.summary.contains("Reading machine reports"))
        state=state.stage(1,"one",CatalogRefreshStage.Applying)
        assertTrue(state.running);assertNull(state.workspaces.first().lastReceivedAt)
        state=state.stage(1,"two",CatalogRefreshStage.Reports)
        assertTrue(state.summary.contains("Second workspace"))
        state=state.finish(1,setOf("one","two"),1000.0)
        assertFalse(state.running);assertEquals(0,state.failures)
        assertTrue(state.summary.contains("received"));assertFalse(state.summary.contains("provider"))
        assertTrue(state.workspaces.all { it.stage==CatalogRefreshStage.Current && it.lastReceivedAt==1000.0 })
    }
    @Test fun failedWorkspaceKeepsItsOwnLastSuccessAndDoesNotHideOtherSuccess() {
        val previous=CatalogRefreshState().begin(1,workspaces).finish(1,setOf("one","two"),1000.0)
        val result=previous.begin(2,workspaces).stage(2,"two",CatalogRefreshStage.Failed).finish(2,setOf("one"),2000.0)
        assertEquals(1,result.failures);assertTrue(result.summary.contains("Saved values kept"))
        assertEquals(2000.0,result.workspaces[0].lastReceivedAt!!,0.0)
        assertEquals(1000.0,result.workspaces[1].lastReceivedAt!!,0.0)
        assertEquals(CatalogRefreshStage.Failed,result.workspaces[1].stage)
    }
    @Test fun staleGenerationsCannotPublishSuccessErrorOrStopANewerRequest() {
        val current=CatalogRefreshState().begin(1,workspaces).begin(2,workspaces)
        assertSame(current,current.stage(1,"one",CatalogRefreshStage.Failed))
        assertSame(current,current.finish(1,setOf("one","two"),5000.0))
        val done=current.finish(2,setOf("one","two"),1000.0)
        assertSame(done,done.stage(2,"one",CatalogRefreshStage.Reports))
        assertSame(done,done.finish(2,emptySet(),6000.0))
    }
    @Test fun cancellationIsInterruptedAndNeverAnUpdatedTimestamp() {
        val result=CatalogRefreshState().begin(1,workspaces).stage(1,"one",CatalogRefreshStage.Applying).finish(1,emptySet(),1000.0)
        assertEquals(2,result.failures);assertTrue(result.workspaces.all { it.stage==CatalogRefreshStage.Interrupted && it.lastReceivedAt==null })
    }
    @Test fun removedAndAddedWorkspacesNeverInheritAnotherWorkspacesAge() {
        val old=CatalogRefreshState().begin(1,workspaces).finish(1,setOf("one","two"),1000.0)
        val next=old.begin(2,listOf("two" to "Renamed","three" to "New workspace"))
        assertEquals(listOf("two","three"),next.workspaces.map { it.id })
        assertEquals(1000.0,next.workspaces[0].lastReceivedAt!!,0.0);assertNull(next.workspaces[1].lastReceivedAt)
    }
}
