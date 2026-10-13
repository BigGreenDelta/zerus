package app.zerus.mobile

import org.json.JSONObject

object NativeAttachments {
    fun reason(target:Target,raw:JSONObject?):String = when {
        raw?.has("attachments_supported")==true && !raw.optBoolean("attachments_supported") ->
            raw.string("attachments_reason").ifBlank { "This native adapter does not support file attachments." }
        target.session.substringBefore('/')=="opencode" -> "OpenCode file attachments are not supported yet. Your existing files remain saved; remove them to send text."
        else -> ""
    }
}
