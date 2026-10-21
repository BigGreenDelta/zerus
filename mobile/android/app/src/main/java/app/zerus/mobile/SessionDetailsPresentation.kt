package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject

data class DetailTask(val id: String, val title: String, val status: String, val owner: String)
data class DetailTaskList(val owner: String, val label: String, val tasks: List<DetailTask>, val recorded: Boolean, val truncated: Boolean)
data class DetailAgent(val id: String, val label: String, val state: String, val task: String, val preview: String,
    val group: Boolean, val recorded: Boolean, val active: Long?, val total: Long?)
data class DetailProcess(val id: String, val command: String, val description: String, val status: String,
    val owner: String, val directory: String, val run: String, val conversation: String, val stale: Boolean,
    val output: Boolean, val stop: Boolean)
data class SessionDetailsView(val fields: List<ContextDetail>, val goal: List<ContextDetail>, val goalNotice: String,
    val tasks: List<DetailTaskList>, val agents: List<DetailAgent>, val agentNotice: String,
    val processes: List<DetailProcess>, val processNotes: List<String>, val processComplete: Boolean,
    val generation: String, val recorded: Boolean)
/** [effortLabels] maps native effort IDs to labels the computer reported (OpenCode variants). */
data class NativeModelChoice(val id: String, val label: String, val efforts: List<String>, val effortLabels: Map<String, String> = emptyMap(),
    val provider:String="",val providerLabel:String="",val free:Boolean=false,val favorite:Boolean=false,val isDefault:Boolean=false)
data class NativeSettings(val model: String, val effort: String, val models: List<NativeModelChoice>, val efforts: List<String>,
    val pendingModel: String, val pendingEffort: String, val pendingId: String, val applyWhen: String,
    val supported: Boolean, val reason: String, val notice: String = "",val modelsTruncated:Boolean=false) {
    /** Relevant semantic fields only; timestamps and unrelated history polls do not invalidate a dialog. */
    val signature get() = JSONArray(listOf(model, effort, models.map { listOf(it.id, it.label, it.efforts, it.effortLabels.entries.map { entry -> listOf(entry.key, entry.value) },it.provider,it.providerLabel,it.free,it.favorite,it.isDefault) }, efforts,
        pendingModel, pendingEffort, pendingId, applyWhen, supported, reason, notice,modelsTruncated)).toString()
}

object SessionDetailsPresentation {
    private fun numeric(raw: JSONObject, key: String): Long? = (raw.opt(key) as? Number)?.toDouble()?.takeIf {
        it.isFinite() && it >= 0 && it <= 9_007_199_254_740_991.0
    }?.toLong()
    private fun strings(raw: JSONArray?): List<String> = raw?.let { values -> (0 until values.length()).mapNotNull { values.opt(it) as? String } }.orEmpty()
    fun settings(raw: JSONObject?): NativeSettings {
        val options = raw?.optJSONArray("model_options")
        // Older inspections may omit agent but still report the new per-model provider fields.
        val openCode = raw?.string("agent") == "opencode" || options?.objects().orEmpty().any {
            (it.opt("provider") as? String)?.isNotBlank() == true
        }
        val models = if (openCode) OpenCodeCatalog.models(options).map { model ->
            NativeModelChoice(model.value, model.label, model.efforts.map { it.value },
                model.efforts.mapNotNull { effort -> effort.label?.let { effort.value to it } }.toMap(),
                model.provider, model.providerLabel, model.free, model.favorite, model.isDefault)
        } else options?.objects().orEmpty().map { option -> NativeEfforts.parse(option).let { efforts ->
            NativeModelChoice(option.string("id"), option.string("label").ifBlank { option.string("id") }, efforts.map { it.value },
                efforts.mapNotNull { effort -> effort.label?.let { effort.value to it } }.toMap())
        } }
        return NativeSettings(raw?.string("settings_model").orEmpty().ifBlank { raw?.string("model").orEmpty() }, raw?.string("effort").orEmpty(), models,
            NativeEfforts.parse(raw).map { it.value }, raw?.string("pending_model").orEmpty(), raw?.string("pending_effort").orEmpty(),
            raw?.string("pending_settings_id").orEmpty(), raw?.string("settings_apply_when").orEmpty(), raw?.optBoolean("settings_change_supported") == true,
            raw?.string("settings_change_reason").orEmpty(), raw?.string("settings_change_notice").orEmpty(),
            openCode && (raw?.opt("model_options_truncated") == true || (options?.length() ?: 0) > OpenCodeCatalog.MAX_MODELS))
    }

    /**
     * Effort page rows for [model]: Default first, then that model's own options, else the session's list.
     * Without [nativeNames] (relay and connector `opencode_launch_effort`) only names every relay accepts remain.
     */
    fun effortChoices(settings: NativeSettings, model: String, nativeNames: Boolean): List<String> =
        (listOf("") + (settings.models.find { it.id == model }?.efforts ?: settings.efforts)).distinct()
            .filter { nativeNames || it in SessionActionPolicies.relayEfforts }
    /** The computer's label for this model's effort ([NativeEfforts]), else the shared name. */
    fun effortLabel(settings: NativeSettings, model: String, effort: String): String =
        effort.takeIf { it.isNotEmpty() }?.let { settings.models.find { choice -> choice.id == model }?.effortLabels?.get(it) } ?: effortLabel(effort)
    fun effortLabel(effort: String) = when(effort) {
        "" -> "Default"
        "xhigh" -> "Extra high"
        else -> effort.replaceFirstChar { it.uppercase() }
    }

    fun parse(raw: JSONObject?, target: Target, verified: Boolean): SessionDetailsView {
        val record = raw ?: JSONObject()
        val recorded = !verified || target.archiveId.isNotBlank()
        val fields = buildList {
            add(ContextDetail("Session", target.session))
            SessionReference.identifiers(target).forEach { identifier ->
                add(ContextDetail(identifier.substringBefore(": "), identifier.substringAfter(": ")))
            }
            if (target.archiveId.isNotBlank()) add(ContextDetail("Archive ID", target.archiveId))
            listOf("agent" to "Provider", "model" to "Model", "effort" to "Effort", "cwd" to "Directory", "project" to "Project").forEach { (key, label) ->
                record.string(key).takeIf { it.isNotBlank() }?.let { add(ContextDetail(label, it)) }
            }
            add(ContextDetail("Process", record.string("process_state").ifBlank { "Not reported" }))
            add(ContextDetail("Activity", record.string("activity").ifBlank { "Not reported" }))
            record.string("error").takeIf { it.isNotBlank() }?.let { add(ContextDetail("Native error", it)) }
        }
        val goalRaw = record.optJSONObject("goal")
        val goal = buildList {
            if (goalRaw != null && goalRaw.string("objective").isNotBlank()) {
                add(ContextDetail("Goal", goalRaw.string("objective")))
                add(ContextDetail("Status", goalStatus(goalRaw)))
                listOf("completion_criterion" to "Completion criterion", "reason" to "Reason").forEach { (key,label) ->
                    goalRaw.string(key).takeIf { it.isNotBlank() }?.let { add(ContextDetail(label,it)) }
                }
                numeric(goalRaw, "tokens_used")?.let { used -> add(ContextDetail("Tokens", "$used" + (numeric(goalRaw,"token_budget")?.takeIf { it > 0 }?.let { " / $it" } ?: " used"))) }
                numeric(goalRaw,"time_used_seconds")?.let { add(ContextDetail("Recorded active time", elapsed(it))) }
                numeric(goalRaw,"time_budget_seconds")?.let { add(ContextDetail("Time budget", elapsed(it))) }
                listOf(Triple("rounds_used","round_budget","Goal rounds"), Triple("turns_used","turn_budget","Continuation turns")).forEach { (used,budget,label) ->
                    numeric(goalRaw,used)?.let { value -> add(ContextDetail(label,"$value" + (numeric(goalRaw,budget)?.let { " / $it" } ?: ""))) }
                }
                numeric(goalRaw,"evaluations")?.let { add(ContextDetail("Completion checks","$it")) }
            }
        }
        val roster = record.optJSONObject("subagents") ?: JSONObject()
        val taskLists = record.optJSONObject("task_lists") ?: JSONObject()
        val tasks = taskLists.keys().asSequence().toList().sortedWith(compareBy<String> { it != "main" }.thenBy { it }).mapNotNull { owner ->
            val list = taskLists.optJSONObject(owner) ?: return@mapNotNull null
            val label = if (owner.startsWith("agent:")) roster.optJSONObject(owner.removePrefix("agent:"))?.string("label","name").orEmpty().ifBlank { owner.removePrefix("agent:") }
                else if (list.string("source").startsWith("Task")) "Session tasks" else "Main agent"
            DetailTaskList(owner,label,list.optJSONArray("items")?.objects().orEmpty().map { DetailTask(it.string("id"),it.string("title"),it.string("status"),it.string("owner")) },
                recorded || list.string("run_id") != target.run,list.optBoolean("truncated"))
        }
        val groups = record.string("subagent_source") == "hook_profiles"
        val agentRows = if (groups) record.optJSONObject("subagent_groups") ?: JSONObject() else roster
        val rows = agentRows.keys().asSequence().mapNotNull { id -> agentRows.optJSONObject(id)?.let { id to it } }.toList().ifEmpty {
            if (groups) emptyList() else record.optJSONArray("subagent_previews")?.objects().orEmpty().mapNotNull { child -> child.string("id","agent_id").takeIf { it.isNotBlank() }?.let { it to child } }
        }
        val agents = rows.map { (id,child) -> DetailAgent(id,child.string("label","name","profile").ifBlank { id }, child.string("display_state","state").ifBlank { "unknown" },
            child.string("task","description"),child.string("preview","last_message"),groups,recorded || child.string("run_id").let { it.isNotBlank() && it != target.run },
            numeric(child,"active_count"),numeric(child,"total_count")) }
        val processesRaw = record.optJSONObject("processes") ?: JSONObject()
        val processes = processesRaw.optJSONArray("items")?.objects().orEmpty().map { process ->
            DetailProcess(process.string("id"),process.string("command"),process.string("description"),process.string("status").ifBlank { "unknown" },
                process.string("owner"),process.string("cwd"),process.string("run_id"),process.string("conversation_id"),
                recorded || process.optBoolean("stale") || process.string("run_id") != target.run || process.string("conversation_id") != target.conversation,
                process.optJSONObject("capabilities")?.optBoolean("output") == true,process.optJSONObject("capabilities")?.optBoolean("stop") == true)
        }
        return SessionDetailsView(fields,goal,if (goal.isNotEmpty()) "Reported by the agent. A finished response does not complete this goal." else if (record.string("goal_source") == "unavailable") "Goal data is unavailable for this conversation or agent version." else "No goal reported.",
            tasks,agents,if (groups) "Observed runs grouped by profile. Individual agent identities are not reported." else if (record.string("subagent_source") == "unavailable") "Subagent activity is unavailable for this session." else "",
            processes,strings(processesRaw.optJSONArray("notes")),processesRaw.optBoolean("complete"),processesRaw.string("generation").ifBlank { record.string("generation") },recorded)
    }
    fun taskStatus(status: String) = when(status) { "completed" -> "Done"; "in_progress" -> "In progress"; "pending" -> "Planned"; else -> status.ifBlank { "Unknown" } }
    fun agentStatus(status: String) = when(status) {
        "finished","ready","idle" -> "Ready"; "working","running","busy" -> "Working"; "approval" -> "Needs approval"; "input" -> "Needs input"
        "attention" -> "Needs attention"; "error","failed" -> "Error"; "completed" -> "Finished"; "stopped" -> "Stopped"; "paused" -> "Paused"; else -> status.ifBlank { "Unknown" }
    }
    fun goalStatus(goal: JSONObject) = when(goal.string("status")) {
        "active" -> if(goal.string("activation") == "disarmed") "Goal waiting to resume" else "Pursuing goal"
        "paused" -> "Goal paused"; "blocked" -> "Goal blocked"; "usage_limited" -> "Goal usage limit reached"
        "budget_limited" -> "Goal token budget reached"; "complete" -> "Goal complete"; else -> "Goal status unknown" + goal.string("status").takeIf { it.isNotBlank() }?.let { " ($it)" }.orEmpty()
    }
    private fun elapsed(seconds: Long) = when {
        seconds >= 86400 -> "${seconds / 86400}d ${seconds / 3600 % 24}h"
        seconds >= 3600 -> "${seconds / 3600}h ${seconds / 60 % 60}m"
        seconds >= 60 -> "${seconds / 60}m"
        else -> "${seconds}s"
    }
}
