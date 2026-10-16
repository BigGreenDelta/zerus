package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject

data class QuestionQueue(val selected: Question?, val optional: List<Question>, val index: Int,
    val submitted: List<Question>, val promptCount: Int)

object QuestionPolicies {
    fun submitted(question: Question, target: Target): NativeQuestionDelivery? = question.answerDelivery?.takeIf {
        question.source == "codex_async" && question.optional && it.status == "submitted" &&
            it.questionId == question.id && it.questionHash == question.hash &&
            it.run == question.run && it.conversation == question.conversation &&
            it.run == target.run && it.conversation == target.conversation &&
            question.id.isNotBlank() && question.hash.isNotBlank() && target.run.isNotBlank()
    }
    fun key(question: Question) = question.id + ":" + question.hash

    /**
     * Whether one snapshot `mobile_attention` item is an optional Codex question the agent can
     * continue without. Connectors mark these with `optional: true`. Older connectors send no
     * `optional` key; every `codex-async:` card hgs produces is optional (src/state/codex_questions.rs),
     * so that ID prefix identifies them. Any other item is a blocking request.
     */
    fun optionalAttention(item: JSONObject): Boolean =
        if (item.has("optional")) item.opt("optional") == true
        else (item.opt("question_id") as? String)?.startsWith("codex-async:") == true

    fun queue(questions: List<Question>, target: Target, chosen: String): QuestionQueue {
        val distinct = questions.filter { it.id.isNotBlank() && it.prompts.isNotEmpty() }.distinctBy { it.id }
        val submitted = distinct.filter { submitted(it, target) != null }
        val offered = distinct.filter { it !in submitted }
        val required = offered.filter { !it.optional }.let { cards -> cards.firstOrNull { it.canAnswer } ?: cards.firstOrNull() }
        val optional = offered.filter { it.optional }.sortedByDescending { it.createdAt }
        val index = optional.indexOfFirst { key(it) == chosen }.coerceAtLeast(0)
        return QuestionQueue(required ?: optional.getOrNull(index), optional, index, submitted, offered.sumOf { it.prompts.size })
    }
    fun answers(prompts: List<Prompt>, options: Map<String, Set<String>>, texts: Map<String, String>): JSONArray =
        JSONArray(prompts.map { prompt -> JSONObject().put("question_id", prompt.id)
            // Native source ordering, independent of the order in which checkboxes were tapped.
            .put("selected_option_ids", JSONArray(prompt.choices.filter { it.id in options[prompt.id].orEmpty() }.map { it.id }))
            .put("text", if (prompt.other) texts[prompt.id].orEmpty() else "") })

    fun answerError(prompts: List<Prompt>, options: Map<String, Set<String>>, texts: Map<String, String>): String {
        for (prompt in prompts) {
            val text = texts[prompt.id].orEmpty()
            val selected = options[prompt.id].orEmpty()
            if (text.toByteArray(Charsets.UTF_8).size > 4096 || text.any(Char::isISOControl)) return "Answers must be one line and at most 4096 UTF-8 bytes."
            if (text.trim().startsWith('/') || text.trim().startsWith('!')) return "Enter command-like answers on your computer."
            if (selected.any { id -> prompt.choices.none { it.id == id } }) return "An answer option changed. Review this request again."
            if (text.isNotBlank() && !prompt.other) return "This request does not accept an Other answer."
            if (!prompt.multi && selected.size + (if (text.isNotBlank()) 1 else 0) > 1) return "Choose one answer for this question."
        }
        return ""
    }
    fun complete(prompts: List<Prompt>, options: Map<String, Set<String>>, texts: Map<String, String>): Boolean =
        prompts.isNotEmpty() && answerError(prompts, options, texts).isBlank() && prompts.all {
            options[it.id].orEmpty().isNotEmpty() || it.other && texts[it.id].orEmpty().isNotBlank()
        }

    fun approvalSupported(question: Question) = question.approval && question.prompts.size == 1 &&
        !question.prompts.single().multi && question.prompts.single().choices.any { it.id == "allow" } &&
        question.prompts.single().choices.any { it.id == "deny" }
}
