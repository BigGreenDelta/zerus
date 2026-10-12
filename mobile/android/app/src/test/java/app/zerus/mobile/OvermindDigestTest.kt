package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class OvermindDigestTest {
    private fun task(index: Int) = "%08x-aaaa-4aaa-8aaa-%012x".format(index, index)
    private fun decision(task: String, signature: Any = "sig-$task", title: Any = "Title $task") = JSONObject().put("task", task)
        .put("project", "example").put("title", title).put("signature", signature)
    private fun computer(digest: Any?) = JSONObject().put("id", GATEWAY).put("snapshot", JSONObject().also { if (digest != null) it.put("mobile_overmind", digest) })
    private fun digest(decisions: Any, projects: Any = JSONObject(), schema: Any = 1) = JSONObject().put("schema", schema).put("rev", 42)
        .put("projects", projects).put("decisions", decisions)

    @Test fun digestCarriesProjectBadgesAndDecisionsInOrder() {
        val projects = JSONObject().put("example", JSONObject().put("needs_human", 2).put("backlog", 5))
            .put("other", JSONObject().put("needs_human", 0))
        val parsed = OvermindDigests.parse(computer(digest(JSONArray().put(decision(task(2))).put(decision(task(1))), projects)))!!
        assertEquals(42L, parsed.rev)
        assertEquals(listOf(task(2), task(1)), parsed.decisions.map { it.task })
        assertEquals("Title ${task(2)}", parsed.decisions.first().title)
        assertEquals(2L, parsed.waiting("example"))
        assertEquals(0L, parsed.waiting("missing"))
        assertEquals(mapOf("needs_human" to 2L, "backlog" to 5L), parsed.projects["example"])
        assertEquals("overmind:${task(2)}", OvermindDigests.slot(task(2)))
        assertEquals(task(2), OvermindDigests.task("overmind:${task(2)}"))
        listOf("overmind:", "overmind:not-a-uuid", "overmind:${task(2).uppercase()}", task(2), "session").forEach { assertNull(it, OvermindDigests.task(it)) }
    }

    @Test fun aMissingDigestIsUnknownNotEmpty() {
        assertNull(OvermindDigests.parse(computer(null)))
        assertNull(OvermindDigests.parse(JSONObject().put("id", GATEWAY)))
        assertNull(OvermindDigests.parse(computer(digest(JSONObject()))))
        assertNull(OvermindDigests.parse(computer(digest(JSONArray(), schema = 2))))
        assertNull(OvermindDigests.parse(computer("digest")))
        assertTrue(OvermindDigests.parse(computer(digest(JSONArray())))!!.decisions.isEmpty())
    }

    @Test fun malformedEntriesAreSkippedAndOnlyTheFirstFiftyCount() {
        val rows = JSONArray().put("not an object").put(decision("not-a-uuid")).put(decision(task(1).uppercase()))
            .put(decision(task(3), signature = "")).put(decision(task(4), signature = "x".repeat(129))).put(decision(task(5), signature = "bad\u007f"))
            .put(decision(task(6), signature = 7)).put(decision(task(7))).put(decision(task(7), signature = "second"))
            .put(decision(task(8), title = "Line\nbreak‮" + "x".repeat(300)))
        (9..60).forEach { rows.put(decision(task(it))) }
        val parsed = OvermindDigests.parse(computer(digest(rows, JSONObject().put("bad", "counts").put("example", JSONObject().put("needs_human", -1).put("ready", 1.5).put("done", 3)))))!!
        assertEquals(listOf(task(7), task(8)) + (9..48).map(::task), parsed.decisions.map { it.task })
        assertEquals("sig-${task(7)}", parsed.decisions.first().signature)
        val title = parsed.decisions[1].title
        assertTrue(title.length <= 120 && title.none { it.isISOControl() || it == '‮' })
        assertEquals(mapOf("done" to 3L), parsed.projects["example"])
        assertNull(parsed.projects["bad"])
    }
}
