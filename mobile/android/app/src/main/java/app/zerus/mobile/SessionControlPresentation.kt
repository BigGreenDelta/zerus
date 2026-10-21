package app.zerus.mobile

import org.json.JSONObject

object SessionControlPresentation {
    /** Native presentation decisions; exact-identity authorization remains owned by the VM. */
    fun reason(target: Target, operation: String, raw: JSONObject?): String {
        if(operation == SessionNotificationPreferences.operation) return SessionNotificationPreferences.reason(target,raw)
        val record = raw ?: return "Refresh native session details."
        if (target.agentId.isNotBlank()) return "Session controls belong to the parent conversation."
        val allowed = record.optJSONArray("allowed_actions") ?: return "The computer did not report native action availability. Refresh its session details."
        if ((0 until allowed.length()).any { allowed.opt(it) == operation }) return ""
        return record.optJSONObject("action_reasons")?.string(operation).orEmpty().ifBlank { "This native action is unavailable in the current session state." }
    }
}
