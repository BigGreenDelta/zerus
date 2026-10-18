package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject

data class QuestionTextField(val label: String, val maxLength: Int?)
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
    private val authoredSources=setOf("hook","codex_async","kimi_wire","kimi_plan_review","dsh_native","opencode_native","claude_native_question")
    /** Terminal panels and approval/trust disclosures preserve the exact reported text. */
    fun authoredMarkdown(question:Question)=!question.approval && !question.trustRequest && question.source in authoredSources
    /** A card needs items to be answered; an unanswerable one without items still shows its reason. */
    fun shown(question: Question) = question.id.isNotBlank() && (question.prompts.isNotEmpty() || !question.canAnswer)

    /**
     * Whether one snapshot `mobile_attention` item is an optional Codex question the agent can
     * continue without. Connectors mark these with `optional: true`. Older connectors send no
     * `optional` key; every `codex-async:` card hgs produces is optional (src/state/codex_questions.rs),
     * so that ID prefix identifies them. Any other item is a blocking request.
     */
    fun optionalAttention(item: JSONObject): Boolean =
        if (item.has("optional")) item.opt("optional") == true
        else (item.opt("question_id") as? String)?.startsWith("codex-async:") == true

    /**
     * Pending requests in one listing row that the agent cannot continue without: the connector's
     * `mobile_attention` items other than optional Codex questions or, without that list, the older
     * `pending_question_count`. As on the desktop, optional questions are never attention; a row
     * whose only pending items are optional has none.
     */
    fun blockingRequests(raw: JSONObject): Int =
        raw.optJSONArray("mobile_attention")?.objects()?.count { !optionalAttention(it) } ?: raw.optInt("pending_question_count")

    fun queue(questions: List<Question>, target: Target, chosen: String): QuestionQueue {
        val distinct = questions.filter(::shown).distinctBy { it.id }
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
            if (text.isNotEmpty()) {
                prompt.maxLength?.let { if (characters(text) > it) return "Answer is longer than $it characters" }
                prompt.minLength?.let { if (characters(text) < it) return "Answer is shorter than $it characters" }
            }
            if (prompt.multi) {
                val count = selected.size + (if (text.isNotBlank()) 1 else 0)
                prompt.maxItems?.let { if (count > it) return "Choose at most $it answers" }
                prompt.minItems?.let { if (count in 1 until it) return "Choose at least $it answers" }
            }
        }
        return ""
    }
    fun complete(prompts: List<Prompt>, options: Map<String, Set<String>>, texts: Map<String, String>): Boolean =
        prompts.isNotEmpty() && answerError(prompts, options, texts).isBlank() && prompts.all {
            options[it.id].orEmpty().isNotEmpty() || it.other && texts[it.id].orEmpty().isNotBlank() || mayBeEmpty(it)
        }
    /**
     * A form item (one with the additive `input_kind`) that is not required: its ordinary empty answer row
     * leaves the field out on the computer. Older items keep requiring an answer or the card's Skip.
     */
    fun mayBeEmpty(prompt: Prompt) = prompt.inputKind.isNotEmpty() && !prompt.required

    /**
     * The free-text field of one item, or null when it takes only offered options. A plain text item
     * (`input_kind: "text"`) is the answer itself. An unanswerable card is read-only and has no field.
     */
    fun textField(question: Question, prompt: Prompt): QuestionTextField? = when {
        !prompt.other || question.approval || !question.canAnswer -> null
        prompt.inputKind == "text" -> QuestionTextField(prompt.otherLabel.ifBlank { "Answer" }, prompt.maxLength)
        else -> QuestionTextField(prompt.otherLabel.ifBlank { "Other answer" }, prompt.maxLength)
    }
    /** Characters as the computer counts them: Unicode code points. */
    fun characters(text: String) = text.codePointCount(0, text.length)
    fun limitText(text: String, max: Int?): String =
        if (max == null || characters(text) <= max) text else text.substring(0, text.offsetByCodePoints(0, max))
    /** Saved selections win; otherwise the item's offered defaults (one for a single choice). */
    fun initialOptions(prompts: List<Prompt>, saved: Map<String, NativeQuestionAnswer>): Map<String, Set<String>> =
        prompts.associate { prompt -> prompt.id to (saved[prompt.id]?.optionIds?.toSet()
            ?: prompt.defaults.filter { id -> prompt.choices.any { it.id == id } }.let { if (prompt.multi) it else it.take(1) }.toSet()) }
    /** The draft of this exact question (target, question ID and hash); a changed question has none. */
    fun savedDraft(drafts: List<Draft>, target: Target, question: Question?): Draft? {
        val questionId = question?.id.orEmpty(); val hash = question?.hash.orEmpty()
        return drafts.find { it.target == target && it.questionId == questionId && it.questionHash == hash && it.detachedId.isBlank() }
    }
    /** Answer rows saved in a draft, by item ID. */
    fun savedAnswers(answers: String): Map<String, NativeQuestionAnswer> = runCatching { JSONArray(answers).objects().associate { answer ->
        answer.string("question_id") to NativeQuestionAnswer(answer.string("question_id"), answer.optJSONArray("selected_option_ids")?.let { ids ->
            (0 until ids.length()).mapNotNull { ids.opt(it) as? String }
        }.orEmpty(), answer.string("text"), answer.optBoolean("skip"))
    } }.getOrDefault(emptyMap())
    /** A saved answer wins, even an intentionally empty one; otherwise the item's native default text. Nothing is sent. */
    fun initialTexts(prompts: List<Prompt>, saved: Map<String, NativeQuestionAnswer>): Map<String, String> =
        prompts.associate { it.id to (saved[it.id]?.text ?: it.defaultText) }
    /** A native default bounded like typed text: a one-line string clipped to [max] characters and 4096 UTF-8 bytes, else none. */
    fun defaultText(raw: Any?, max: Int?): String = (raw as? String)?.takeIf { it.none(Char::isISOControl) }
        ?.let { limitText(it, max) }?.takeIf { it.toByteArray(Charsets.UTF_8).size <= 4096 }.orEmpty()
    /** An unanswerable card shows its reason and no Send. */
    fun submitVisible(question: Question) = question.canAnswer
    /** Options and text accept edits only while the draft is editable and the card can be answered. */
    fun inputEnabled(question: Question, editing: Boolean) = editing && question.canAnswer
    fun unavailableNotice(question: Question) = question.unavailableReason.ifBlank { "Complete this request on your machine." }

    fun approvalSupported(question: Question) = question.approval && question.prompts.size == 1 &&
        !question.prompts.single().multi && question.prompts.single().choices.any { it.id == "allow" } &&
        question.prompts.single().choices.any { it.id == "deny" }
}
