package app.zerus.mobile

import org.junit.Assert.*
import org.junit.Test

class ConnectionRemovalTest {
    private enum class Fault { Key, Journal, Index }
    /** Fault boundaries of the platform journal, with no Android Keystore or owner state. */
    private class Fixture(var fault:Fault?) {
        val protected=Connection("protected","Synthetic","https://relay.example","synthetic",2)
        val other=Connection("other","Synthetic","https://relay.example","synthetic",2)
        var durable=listOf(protected,other)
        var visible=durable
        var keyPresent=true
        var journalPresent=true
        fun forget() {
            visible=ConnectionRemoval.forget(protected,visible,{ connection ->
                check(connection==protected)
                if(fault==Fault.Key) error("Synthetic key deletion failure")
                keyPresent=false // Deleting an already missing key succeeds on retry.
                if(fault==Fault.Journal) error("Synthetic journal transaction failure")
                journalPresent=false
            },{ next ->
                if(fault==Fault.Index) error("Synthetic connection index failure")
                durable=next
            })
        }
        fun retryAfterReload() {
            visible=durable
            fault=null
            forget()
            assertEquals(listOf(other),visible)
            assertEquals(visible,durable)
            assertFalse(keyPresent);assertFalse(journalPresent)
        }
    }

    @Test fun keyDeletionFailureKeepsConnectionVisibleAndDurableForRetry() {
        val fixture=Fixture(Fault.Key)
        assertTrue(runCatching { fixture.forget() }.isFailure)
        assertEquals(fixture.durable,fixture.visible)
        assertTrue(fixture.visible.contains(fixture.protected))
        assertTrue(fixture.keyPresent);assertTrue(fixture.journalPresent)
        fixture.retryAfterReload()
    }

    @Test fun journalFailureAfterKeyDeletionKeepsRetryWithoutCreatingKeys() {
        val fixture=Fixture(Fault.Journal)
        assertTrue(runCatching { fixture.forget() }.isFailure)
        assertEquals(fixture.durable,fixture.visible)
        assertTrue(fixture.visible.contains(fixture.protected))
        assertFalse(fixture.keyPresent);assertTrue(fixture.journalPresent)
        fixture.retryAfterReload()
    }

    @Test fun finalIndexFailureKeepsRetryAfterProtectedMaterialAlreadyRemoved() {
        val fixture=Fixture(Fault.Index)
        assertTrue(runCatching { fixture.forget() }.isFailure)
        assertEquals(fixture.durable,fixture.visible)
        assertTrue(fixture.visible.contains(fixture.protected))
        assertFalse(fixture.keyPresent);assertFalse(fixture.journalPresent)
        fixture.retryAfterReload()
    }

    @Test fun legacyRemovalSkipsProtectedCleanupAndChangedConnectionRefusesBeforeEffects() {
        val legacy=Connection("legacy","Synthetic","https://relay.example","synthetic")
        var saved=false
        assertEquals(emptyList<Connection>(),ConnectionRemoval.forget(legacy,listOf(legacy),{ error("Legacy must not erase protected keys") },{ saved=true }))
        assertTrue(saved);saved=false
        val changed=legacy.copy(token="changed")
        assertTrue(runCatching { ConnectionRemoval.forget(legacy,listOf(changed),{ error("Must not erase a changed connection") },{ saved=true }) }.isFailure)
        assertFalse(saved)
    }
}
