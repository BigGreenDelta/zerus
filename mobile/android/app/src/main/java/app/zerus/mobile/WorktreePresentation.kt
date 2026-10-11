package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject

data class LaunchWorktree(val path: String, val branch: String, val kind: String)
object WorktreePresentation {
    fun healthy(raw: JSONObject?) = raw?.string("state") == "ok" && raw.opt("stale") == false &&
        LaunchPresentation.absoluteFolder(raw.string("common_dir")) && LaunchPresentation.absoluteFolder(raw.string("path"))
    fun entries(raw: JSONObject?): List<LaunchWorktree> = if (!healthy(raw)) emptyList() else
        raw?.optJSONArray("worktrees")?.objects().orEmpty().mapNotNull { row ->
            val path = row.opt("path") as? String
            val kind = row.opt("kind") as? String
            if (path == null || !LaunchPresentation.absoluteFolder(path) || row.opt("available") != true || kind !in setOf("main", "linked")) null
            else LaunchWorktree(path, row.string("branch"), kind!!)
        }.distinctBy { it.path }.take(512)
    private fun within(path: String, root: String) = path == root
    private fun inside(path: String, root: String) = LaunchPresentation.absoluteFolder(root) &&
        (path == root || path.startsWith(if (root == "/") root else "$root/"))
    fun contains(raw: JSONObject?, path: String) = entries(raw).any { within(path, it.path) }
    /** The catalog read for this exact selection: its requested or canonical folder. */
    fun forFolder(raw: JSONObject?, folder: String): JSONObject? =
        raw?.takeIf { folder.isNotBlank() && (it.string("requested_path") == folder || it.string("path") == folder) }
    fun anchor(raw: JSONObject?, project: LaunchProject, directory: String): LaunchProjectFolder? {
        if (!healthy(raw) || !contains(raw, directory)) return null
        return project.folders.firstOrNull { folder -> contains(raw, folder.path) ||
            folder.path == raw?.string("requested_path") && contains(raw, raw.string("path")) }
    }
    /** Desktop automaticWorktreeAvailable: a fresh, complete catalog of this selection inside a named, available checkout. */
    fun automaticAvailable(raw: JSONObject?, folder: String): Boolean {
        val catalog = forFolder(raw, folder) ?: return false
        if (catalog.string("state") != "ok" || catalog.opt("stale") != false || catalog.opt("partial") == true ||
            !LaunchPresentation.absoluteFolder(catalog.string("common_dir"))) return false
        if (automaticRoot(catalog).isEmpty()) return false
        val selected = catalog.string("path").ifBlank { folder }
        return catalog.optJSONArray("worktrees")?.objects().orEmpty().any { row ->
            row.opt("available") == true && row.string("kind") in setOf("main", "linked") && inside(selected, row.string("path"))
        }
    }
    private fun automaticRoot(raw: JSONObject?): String {
        val root = raw?.string("selected_root").orEmpty().trimEnd('/')
        return if (LaunchPresentation.absoluteFolder(root) && root.substringAfterLast('/').isNotEmpty()) root else ""
    }
    /** The generated sibling checkout `<root>-<branch>` that Auto creates when the session starts. */
    fun automaticPath(raw: JSONObject?, branch: String): String {
        val root = automaticRoot(raw)
        return if (root.isEmpty() || branch.isBlank()) "" else sibling(root, branch)
    }
    /** Explains why Auto is unavailable for the selected folder; "" when it is available or nothing is selected. */
    fun automaticNote(raw: JSONObject?, folder: String, supported: Boolean, loading: Boolean): String {
        val catalog = forFolder(raw, folder)
        return when {
            folder.isBlank() -> ""
            !supported -> "Update Zerus on this computer to create worktrees. The session starts in this folder."
            loading -> "Checking repository…"
            automaticAvailable(catalog, folder) -> ""
            catalog?.string("state") == "not_repo" -> "Not a Git repository. The session starts in this folder."
            else -> "Repository details are unavailable. The session starts in this folder."
        }
    }
    /** Creation evidence retained by the form: the confirmed checkout joins the repository it was created from. */
    fun withCreated(raw: JSONObject, path: String): JSONObject {
        val copy = JSONObject(raw.toString())
        val rows = copy.optJSONArray("worktrees") ?: JSONArray().also { copy.put("worktrees", it) }
        if (rows.objects().none { it.string("path") == path })
            rows.put(JSONObject().put("path", path).put("kind", "linked").put("available", true))
        return copy
    }
    /** Fresh evidence first; retained creation evidence only when it alone proves a related project folder. */
    fun placement(fresh: JSONObject?, created: JSONObject?, project: LaunchProject?, directory: String): JSONObject? {
        if (project == null) return fresh
        return listOfNotNull(fresh, created).firstOrNull { anchor(it, project, directory) != null } ?: fresh
    }
    fun createError(branch: String, base: String, destination: String): String = when {
        branch.isBlank() || base.isBlank() -> "Enter a new branch and starting revision."
        listOf(branch, base).any { it.trim() != it || it.startsWith('-') || it.length > 256 || it.any(Char::isISOControl) } -> "Use a valid branch and revision."
        !LaunchPresentation.absoluteFolder(destination) -> "Choose an absolute new folder path."
        else -> ""
    }
    fun destination(raw: JSONObject?, branch: String): String {
        val root = raw?.string("selected_root").orEmpty().ifBlank { raw?.string("path").orEmpty() }.trimEnd('/')
        if (!LaunchPresentation.absoluteFolder(root)) return ""
        return sibling(root, branch)
    }
    private fun sibling(root: String, branch: String): String {
        val safe = branch.map { if (it.isLetterOrDigit() || it in "._-") it else '-' }.joinToString("")
        return "$root-$safe"
    }
}
