package app.zerus.mobile

import org.json.JSONArray

/** Explicit answer intent retained by the ViewModel, never persisted as consent. */
internal data class QuestionSendIntent(val target: Target, val navigation: Long, val question: Question,
    val draft: Draft, val answers: String) {
    fun matches(target: Target?, navigation: Long, current: Question?, draft: Draft): Boolean =
        this.target == target && this.navigation == navigation && current != null &&
            QuestionSendPolicies.schema(question) == QuestionSendPolicies.schema(current) &&
            QuestionSendPolicies.available(current, this.target, answers) && ContextPolicies.sameDraft(this.draft, draft)
    fun ownsPrepared(prepared: Draft, requestId: String) = requestId.isNotBlank() &&
        prepared.requestId == requestId && prepared.status == "submitting" &&
        ContextPolicies.sameDraft(draft.copy(answers = answers), prepared.copy(status = "editing"))
    fun restoreAfterExpiry(state: MessageState, requestId: String) = state.copy(drafts = state.drafts.map {
        if (ownsPrepared(it, requestId)) draft else it
    })
    fun permitsPrepared(target: Target?, navigation: Long, current: Question?, prepared: Draft,
        requestId: String, cold: Boolean, consent: Boolean): Boolean =
        requestId.isNotBlank() && prepared.requestId == requestId && prepared.status == "submitting" &&
            (!QuestionSendPolicies.requiresCacheConsent(question, cold) || consent) && matches(target, navigation, current,
                prepared.copy(status = "editing", answers = draft.answers)) && prepared.answers == answers
}

internal object QuestionSendPolicies {
    /** Startup cards can share an empty conversation; every card still belongs to one exact nonblank run. */
    fun bound(question: Question, target: Target) = target.run.isNotBlank() &&
        question.run == target.run && question.conversation == target.conversation
    // Kimi's reviewed native chooser already asks how to handle its cold cache.
    fun requiresCacheConsent(question: Question, cold: Boolean) = cold &&
        !(question.source == "kimi_cache_hint" && question.toolCallId == "kimi_cache_hint")
    fun schema(question: Question) = question.copy(canAnswer = false, canSkip = false, delivery = "",
        createdAt = 0.0, unavailableReason = "", answerDelivery = null)
    fun capture(target: Target, navigation: Long, question: Question, draft: Draft, answers: String) =
        QuestionSendIntent(target, navigation, schema(question).copy(prompts = question.prompts.map {
            it.copy(choices = it.choices.toList())
        }), draft.copy(attachments = draft.attachments.toList()), answers)
    fun available(question: Question, target: Target, answers: String): Boolean {
        if (!bound(question, target)) return false
        if (QuestionPolicies.submitted(question, target) != null) return false
        val rows = runCatching { JSONArray(answers).objects() }.getOrNull() ?: return false
        if (rows.size != question.prompts.size || rows.isEmpty() ||
            rows.map { it.string("question_id") } != question.prompts.map { it.id }) return false
        val skip = rows.all { it.optBoolean("skip", false) }
        return if (skip) question.canSkip else rows.none { it.optBoolean("skip", false) } && question.canAnswer
    }
}

/** Refusals follow only the original target and question, including expanded cards. */
internal data class QuestionSendRefusal(val target: Target, val questionId: String, val questionHash: String, val text: String) {
    fun textFor(target: Target, question: Question) = text.takeIf {
        target == this.target && question.id == questionId && question.hash == questionHash
    }.orEmpty()
}

/** A pending modal cannot be replaced, replayed, or restored after process death. */
internal class QuestionSendGate {
    var pending: QuestionSendIntent? = null; private set
    fun request(intent: QuestionSendIntent, cold: Boolean): QuestionSendIntent? {
        if (pending != null) return null
        if (!QuestionSendPolicies.requiresCacheConsent(intent.question, cold)) return intent
        pending = intent
        return null
    }
    fun active(expected: QuestionSendIntent) = pending === expected
    fun confirm(expected: QuestionSendIntent, valid: Boolean): QuestionSendIntent? {
        if (!active(expected)) return null
        pending = null
        return expected.takeIf { valid }
    }
    fun cancel(expected: QuestionSendIntent) { if (pending === expected) pending = null }
}
