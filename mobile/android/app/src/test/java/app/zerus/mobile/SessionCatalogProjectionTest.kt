package app.zerus.mobile

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class SessionCatalogProjectionTest {
    private val key = MachineKey("workspace", "computer")
    private val native = mapOf(key to setOf("archive_list"))
    private val relay = mapOf(key.connectionId to setOf("archive_list"))
    private fun row(id: String, archived: Boolean = false) = Session(
        Target(key.computerId,"codex/project/$id","run-$id","conversation-$id",key.connectionId,
            if(archived) "archive-$id" else ""), id,"codex","","Project","",0,JSONObject())

    @Test fun pagingReplacesLegacyPreviewOnlyWithBothCapabilities() {
        val live=row("live");val legacy=row("legacy",true);val page=row("page",true)
        val catalog=listOf(live,legacy)
        assertEquals(listOf(live,page),SessionCatalogProjection(catalog,listOf(page),native,relay).prepare())
        assertEquals(catalog,SessionCatalogProjection(catalog,emptyList(),native,emptyMap()).prepare())
        assertEquals(catalog,SessionCatalogProjection(catalog,emptyList(),emptyMap(),relay).prepare())
    }

    @Test fun replacingWindowDropsOldRowsAndPreservesExactRunIdentity() {
        val old=row("page",true);val next=old.copy(target=old.target.copy(archiveId="another",run="another-run"))
        assertEquals(listOf(old),SessionCatalogProjection(emptyList(),listOf(old,old),native,relay).prepare())
        assertEquals(listOf(next),SessionCatalogProjection(emptyList(),listOf(next),native,relay).prepare())
        assertEquals(listOf(old,next),SessionCatalogProjection(emptyList(),listOf(old,next),native,relay).prepare())
    }

    @Test fun staleCatalogPageOrCapabilityCannotPublish() {
        val catalog=listOf(row("live"));val pages=listOf(row("page",true))
        val input=SessionCatalogProjection(catalog,pages,native,relay)
        assertTrue(input.matches(catalog,pages.toList(),native,relay))
        assertFalse(input.matches(catalog.toMutableList(),pages,native,relay))
        assertFalse(input.matches(catalog,emptyList(),native,relay))
        assertFalse(input.matches(catalog,pages,native,emptyMap()))
        assertFalse(input.matches(catalog,pages,emptyMap(),relay))
    }

    @Test fun snapshotEqualityDoesNotTraverseTheLiveCatalog() {
        val catalog=object:AbstractList<Session>() {
            override val size=50_000
            override fun get(index:Int):Session=error("Main-thread catalog traversal")
        }
        val input=SessionCatalogProjection(catalog,emptyList(),native,relay)
        assertEquals(input,SessionCatalogProjection(catalog,emptyList(),native,relay))
        assertTrue(input.matches(catalog,emptyList(),native,relay))
        assertEquals(input.hashCode(),SessionCatalogProjection(catalog,emptyList(),native,relay).hashCode())
    }
}
