package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import java.util.UUID

data class OvermindDecisionDigest(val task: String, val project: String, val title: String, val signature: String)
/** The gateway's small summary in its snapshot: counts per project and stage role, and the tasks waiting in Your Call. */
data class OvermindDigest(val rev: Long?, val projects: Map<String, Map<String, Long>>, val decisions: List<OvermindDecisionDigest>) {
    fun waiting(project: String) = projects[project]?.get("needs_human") ?: 0L
}

/** Reads `mobile_overmind` like the relay: a missing or unreadable digest is unknown, never an empty list. */
object OvermindDigests {
    const val SLOT = "overmind:"
    private const val MAX_DECISIONS = 50
    private val role = Regex("[a-z_]{1,32}")
    fun slot(task: String) = SLOT + task
    /** The task a decision alert or card names, or null for anything else. */
    fun task(session: String): String? = session.takeIf { it.startsWith(SLOT) }?.substring(SLOT.length)?.takeIf(::canonical)
    fun canonical(value: String) = value.length == 36 && runCatching { UUID.fromString(value).toString() == value }.getOrDefault(false)
    private fun clean(value: Any?, max: Int): String {
        val text = value as? String ?: return ""
        val result = StringBuilder(); var count = 0
        text.codePoints().forEach { point ->
            val type = Character.getType(point)
            if (count < max && type != Character.CONTROL.toInt() && type != Character.FORMAT.toInt() && type != Character.SURROGATE.toInt()) { result.appendCodePoint(point); count++ }
        }
        return result.toString()
    }
    private fun signature(value: Any?) = (value as? String)?.takeIf { it.codePointCount(0, it.length) in 1..128 && it.none { c -> c < ' ' || c == '\u007f' } }
    fun parse(computer: JSONObject): OvermindDigest? {
        val raw = computer.optJSONObject("snapshot")?.optJSONObject("mobile_overmind") ?: return null
        if (raw.opt("schema") !is Number || raw.optDouble("schema") != 1.0) return null
        val list = raw.opt("decisions") as? JSONArray ?: return null
        if (raw.toString().toByteArray(Charsets.UTF_8).size > 128 * 1024) return null
        val seen = mutableSetOf<String>()
        val decisions = (0 until minOf(list.length(), MAX_DECISIONS)).mapNotNull { index ->
            val row = list.opt(index) as? JSONObject ?: return@mapNotNull null
            val task = (row.opt("task") as? String)?.takeIf(::canonical) ?: return@mapNotNull null
            val signature = signature(row.opt("signature")) ?: return@mapNotNull null
            if (!seen.add(task)) return@mapNotNull null
            OvermindDecisionDigest(task, (row.opt("project") as? String)?.takeIf { it.length <= 512 }.orEmpty(), clean(row.opt("title"), 120), signature)
        }
        val projects = raw.optJSONObject("projects")?.let { all -> all.keys().asSequence().take(1000).mapNotNull { project ->
            val counts = all.opt(project) as? JSONObject ?: return@mapNotNull null
            if (project.length > 512 || project.any { it.isISOControl() }) return@mapNotNull null
            project to counts.keys().asSequence().filter(role::matches).mapNotNull { name -> OvermindParser.count(counts.opt(name))?.let { name to it } }.toMap()
        }.toMap() }.orEmpty()
        return OvermindDigest(OvermindParser.count(raw.opt("rev")), projects, decisions)
    }
}
