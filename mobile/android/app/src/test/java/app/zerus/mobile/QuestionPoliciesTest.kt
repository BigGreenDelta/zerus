package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class QuestionPoliciesTest {
    private val target = Target("computer", "session", "run", "conversation", "workspace")
    private val prompt = Prompt("q", "Question", listOf(Choice("first", "First", ""), Choice("second", "Second", "")), false, true, false)
    private fun question(id: String = "request", optional: Boolean = false, canAnswer: Boolean = true, at: Double = 1.0) =
        Question(id, "hash-$id", listOf(prompt), canAnswer, true, optional, "", createdAt = at, run = target.run, conversation = target.conversation)
    private fun submitted(): Question {
        val q = question(optional = true).copy(source = "codex_async")
        return q.copy(delivery = "submitted", answerDelivery = NativeQuestionDelivery("uuid", q.id, q.hash,
            target.run, target.conversation, "submitted", listOf(NativeQuestionAnswer("q", listOf("second"), ""))))
    }
    @Test fun parserPreservesFullDisclosureAndDefaultsOtherFalse() {
        val body = "dangerous command\n".repeat(300)
        val raw = JSONObject().put("pending_questions", JSONArray().put(JSONObject()
            .put("question_id", "id").put("question_hash", "hash").put("approval", true).put("trust_request", true)
            .put("created_at", 123.0).put("source", "claude_tool_approval").put("answer_unavailable_reason", "Exact native reason")
            .put("run_id", "r").put("conversation_id", "c").put("tool_call_id", "tool")
            .put("questions", JSONArray().put(JSONObject().put("id", "q").put("question", "Approve?")
                .put("header", "Header").put("body", body).put("other_label", "Custom").put("other_description", "Native custom disclosure")))))
        val parsed = NativeParser.questions(raw).single()
        assertEquals(body, parsed.prompts.single().body)
        assertEquals("Header", parsed.prompts.single().header)
        assertEquals("Native custom disclosure", parsed.prompts.single().otherDescription)
        assertFalse(parsed.prompts.single().other)
        assertTrue(parsed.approval); assertTrue(parsed.trustRequest)
        assertEquals("Exact native reason", parsed.unavailableReason)
        assertEquals(123.0, parsed.createdAt, 0.0)
        assertEquals("tool", parsed.toolCallId)
    }
    @Test fun parserPreservesTypedSubmittedAnswerInNativeOrder() {
        val q = submitted()
        val delivery = JSONObject().put("request_id", "uuid").put("question_id", q.id).put("question_hash", q.hash)
            .put("run_id", target.run).put("conversation_id", target.conversation).put("status", "submitted")
            .put("answers", JSONArray().put(JSONObject().put("question_id", "q").put("selected_option_ids", JSONArray(listOf("second", "first"))).put("text", "literal")))
        val parsed = NativeParser.questions(JSONObject().put("pending_questions", JSONArray().put(JSONObject().put("answer_delivery", delivery)))).single()
        assertEquals(listOf("second", "first"), parsed.answerDelivery!!.answers.single().optionIds)
        assertEquals("literal", parsed.answerDelivery.answers.single().text)
    }
    @Test fun requiredActionableRequestTakesPriorityOverOptionalAndUnavailable() {
        val unavailable = question("unavailable", canAnswer = false)
        val required = question("required")
        val optional = question("optional", optional = true)
        assertEquals(required, QuestionPolicies.queue(listOf(optional, unavailable, required), target, QuestionPolicies.key(optional)).selected)
    }
    @Test fun optionalNewestFirstSelectionSurvivesPollInsertion() {
        val old = question("old", true, at = 1.0)
        val chosen = question("chosen", true, at = 2.0)
        val fresh = question("new", true, at = 3.0)
        val queue = QuestionPolicies.queue(listOf(old, fresh, chosen), target, QuestionPolicies.key(chosen))
        assertEquals(chosen, queue.selected)
        assertEquals(listOf(fresh, chosen, old), queue.optional)
        assertEquals(1, queue.index)
    }
    @Test fun submittedMatchingRequestIsHiddenFromOfferedCountAndRetainsAnswer() {
        val sent = submitted()
        val next = question("next", true)
        val queue = QuestionPolicies.queue(listOf(sent, next), target, "")
        assertEquals(next, queue.selected)
        assertEquals(1, queue.promptCount)
        assertEquals(listOf(sent), queue.submitted)
        assertEquals(listOf("second"), QuestionPolicies.submitted(sent, target)!!.answers.single().optionIds)
    }
    @Test fun nativeSubmissionMustMatchEveryIdentityField() {
        val sent = submitted()
        val delivery = sent.answerDelivery!!
        listOf(delivery.copy(questionId = "other"), delivery.copy(questionHash = "other"), delivery.copy(run = "other"),
            delivery.copy(conversation = "other"), delivery.copy(status = "uncertain")).forEach {
            assertNull(QuestionPolicies.submitted(sent.copy(answerDelivery = it), target))
        }
        assertNull(QuestionPolicies.submitted(sent, target.copy(run = "reused")))
        assertNull(QuestionPolicies.submitted(sent, target.copy(conversation = "new")))
        assertNull(QuestionPolicies.submitted(sent.copy(source = "hook"), target))
        assertNull(QuestionPolicies.submitted(sent.copy(optional = false), target))
    }
    @Test fun questionHashReuseDoesNotSelectReplacedOptionalRequest() {
        val old = question("same", true)
        val replacement = old.copy(hash = "replacement")
        val newest = question("newest", true, at = 5.0)
        assertEquals(newest, QuestionPolicies.queue(listOf(replacement, newest), target, QuestionPolicies.key(old)).selected)
    }
    @Test fun duplicateIdsAndEmptyQuestionsAreNotOfferedTwice() {
        val q = question()
        assertEquals(1, QuestionPolicies.queue(listOf(q, q, q.copy(id = ""), q.copy(id = "empty", prompts = emptyList())), target, "").promptCount)
    }
    @Test fun missingOptionsNeverCreateUnsupportedFreeformAnswer() {
        val unsupported = prompt.copy(choices = emptyList())
        assertFalse(QuestionPolicies.complete(listOf(unsupported), emptyMap(), mapOf("q" to "invented")))
        assertTrue(QuestionPolicies.answerError(listOf(unsupported), emptyMap(), mapOf("q" to "invented")).isNotBlank())
        assertEquals("", QuestionPolicies.answers(listOf(unsupported), emptyMap(), mapOf("q" to "invented")).getJSONObject(0).getString("text"))
    }
    @Test fun multiAnswersUseSourceOptionOrder() {
        val result = QuestionPolicies.answers(listOf(prompt.copy(multi = true)), mapOf("q" to linkedSetOf("second", "first")), emptyMap())
        assertEquals(listOf("first", "second"), result.getJSONObject(0).getJSONArray("selected_option_ids").let { ids -> (0 until ids.length()).map { ids.getString(it) } })
    }
    @Test fun customAnswerByteAndCommandRulesMatchNative() {
        val p = prompt.copy(other = true)
        assertTrue(QuestionPolicies.answerError(listOf(p), emptyMap(), mapOf("q" to "é".repeat(2049))).isNotBlank())
        assertTrue(QuestionPolicies.answerError(listOf(p), emptyMap(), mapOf("q" to "/command")).isNotBlank())
        assertTrue(QuestionPolicies.answerError(listOf(p), emptyMap(), mapOf("q" to "a\nb")).isNotBlank())
        assertTrue(QuestionPolicies.complete(listOf(p), emptyMap(), mapOf("q" to "literal <tag>")))
    }
    private fun card(source: String, items: JSONArray, approval: Boolean = false, canAnswer: Boolean = true, choices: Boolean = false) =
        NativeParser.questions(JSONObject().put("pending_questions", JSONArray().put(JSONObject().put("question_id", "card-$source")
            .put("question_hash", "hash-$source").put("source", source).put("approval", approval).put("approval_choices", choices)
            .put("can_answer", canAnswer).put("questions", items)))).single()
    private fun options(vararg ids: String) = JSONArray(ids.map { JSONObject().put("id", it).put("label", it) })
    private val choiceItem get() = JSONObject().put("id", "c").put("question", "Pick").put("allow_other", true).put("options", options("a", "b"))
    private val textItem get() = JSONObject().put("id", "t").put("question", "Path").put("allow_other", true).put("input_kind", "text")
    private fun permission(id: String = "permission", choices: JSONArray = options("once", "always", "reject"), multi: Boolean = false) =
        JSONObject().put("id", id).put("question", "shell").put("allow_other", false).put("multi_select", multi).put("options", choices)

    @Test fun openCodeFormTextMayStartWithSlashOrBang() {
        val form = card("opencode_native", JSONArray().put(textItem).put(choiceItem))
        val literal = QuestionPolicies.formReplyText(form)
        assertTrue(literal)
        listOf("/tmp/report.txt", "!important", "  /x").forEach { text ->
            // Both the text item and the Other answer of a choice item.
            val texts = mapOf("t" to text, "c" to text)
            assertEquals(text, "", QuestionPolicies.answerError(form.prompts, emptyMap(), texts, literal))
            assertTrue(text, QuestionPolicies.complete(form.prompts, emptyMap(), texts, literal))
            assertTrue(text, QuestionSteps.current(form.prompts, 0, emptyMap(), texts, literal).complete)
        }
        // Every other text rule still applies.
        assertEquals("Answers must be one line and at most 4096 UTF-8 bytes.", QuestionPolicies.answerError(form.prompts, emptyMap(), mapOf("t" to "/a\nb"), literal))
        assertEquals("This request does not accept an Other answer.",
            QuestionPolicies.answerError(listOf(form.prompts.last().copy(other = false)), emptyMap(), mapOf("c" to "/x"), literal))
    }

    @Test fun commandLikeTextIsRefusedOutsideOpenCodeForms() {
        val refused = "Enter command-like answers on your computer."
        val others = listOf("codex_async", "claude_terminal_choice", "claude_tool_approval", "kimi_wire", "dsh_native", "startup_terminal_choice", "")
            .map { card(it, JSONArray().put(choiceItem)) }
        // An OpenCode approval card is not a form.
        val openCodeApproval = card("opencode_native", JSONArray().put(choiceItem), approval = true)
        (others + openCodeApproval).forEach { question ->
            val literal = QuestionPolicies.formReplyText(question)
            assertFalse(question.source, literal)
            listOf("/command", "!rm -rf", "  /x").forEach { text ->
                val texts = mapOf("c" to text)
                assertEquals(question.source + text, refused, QuestionPolicies.answerError(question.prompts, emptyMap(), texts, literal))
                assertFalse(question.source + text, QuestionPolicies.complete(question.prompts, emptyMap(), texts, literal))
                assertFalse(question.source + text, QuestionSteps.current(question.prompts, 0, emptyMap(), texts, literal).complete)
            }
        }
        // A prompt built without its card keeps the refusal.
        assertEquals(refused, QuestionPolicies.answerError(listOf(prompt.copy(other = true)), emptyMap(), mapOf("q" to "/command")))
    }

    @Test fun openCodePermissionCardsQualifyOnlyInTheirExactShape() {
        val full = card("opencode_native", JSONArray().put(permission()), approval = true, choices = true)
        assertTrue(QuestionPolicies.openCodePermission(full))
        assertTrue(QuestionPolicies.alwaysAllowOffered(full))
        // Without Always allow the card still qualifies; only Reject and Allow once are offered.
        val narrow = card("opencode_native", JSONArray().put(permission(choices = options("reject", "once"))), approval = true, choices = true)
        assertTrue(QuestionPolicies.openCodePermission(narrow))
        assertFalse(QuestionPolicies.alwaysAllowOffered(narrow))
        val refused = mapOf(
            "claude approval_choices" to card("claude_tool_approval", JSONArray().put(permission("approval", options("1", "2", "3"))), approval = true, choices = true),
            "claude with OpenCode ids" to card("claude_tool_approval", JSONArray().put(permission()), approval = true, choices = true),
            "allow/deny" to card("opencode_native", JSONArray().put(permission(choices = options("allow", "deny"))), approval = true),
            "extra option" to card("opencode_native", JSONArray().put(permission(choices = options("once", "always", "reject", "later"))), approval = true, choices = true),
            "no once" to card("opencode_native", JSONArray().put(permission(choices = options("always", "reject"))), approval = true, choices = true),
            "no reject" to card("opencode_native", JSONArray().put(permission(choices = options("once", "always"))), approval = true, choices = true),
            "multi" to card("opencode_native", JSONArray().put(permission(multi = true)), approval = true, choices = true),
            "second prompt" to card("opencode_native", JSONArray().put(permission()).put(permission("other")), approval = true, choices = true),
            "other item id" to card("opencode_native", JSONArray().put(permission("approval")), approval = true, choices = true),
            "not an approval" to card("opencode_native", JSONArray().put(permission())),
            "unanswerable" to card("opencode_native", JSONArray().put(permission()), approval = true, canAnswer = false, choices = true),
            "no items" to card("opencode_native", JSONArray(), approval = true, choices = true))
        refused.forEach { (name, question) ->
            assertFalse(name, QuestionPolicies.openCodePermission(question))
            assertFalse(name, QuestionPolicies.alwaysAllowOffered(question))
        }
        // Allow/deny cards behave as before; Claude choice cards are not allow/deny cards (see nativeChoices).
        assertTrue(QuestionPolicies.approvalSupported(refused.getValue("allow/deny")))
        assertFalse(QuestionPolicies.approvalSupported(refused.getValue("claude approval_choices")))
        assertFalse(QuestionPolicies.approvalSupported(full))
    }

    @Test fun approvalButtonsSendOneRowForTheChosenOption() {
        val allowDeny = question().copy(approval = true, prompts = listOf(prompt.copy(id = "approval", choices = listOf(Choice("allow", "Yes", ""), Choice("deny", "No", "")))))
        val row = QuestionPolicies.approvalAnswer(allowDeny, "deny")
        assertEquals(1, row.length())
        assertEquals("approval", row.getJSONObject(0).getString("question_id"))
        assertEquals(listOf("deny"), row.getJSONObject(0).getJSONArray("selected_option_ids").let { ids -> (0 until ids.length()).map(ids::getString) })
        assertEquals("", row.getJSONObject(0).getString("text"))
    }

    @Test fun alwaysAllowNeedsItsOwnConfirmationOfTheSameCard() {
        val question = card("opencode_native", JSONArray().put(permission()), approval = true, choices = true)
        val sent = mutableListOf<JSONArray>()
        // Only after the same review as Allow once.
        assertFalse(AlwaysAllowConfirmation().request(question, reviewed = false).open)
        val open = AlwaysAllowConfirmation().request(question, reviewed = true)
        assertTrue(open.open); assertTrue(open.showing(question, eligible = true))
        // Cancel sends nothing, and a late Confirm after it sends nothing either.
        val cancelled = open.cancel()
        assertFalse(cancelled.open)
        assertFalse(cancelled.confirm(question, eligible = true) { sent += it }.open)
        assertTrue(sent.isEmpty())
        // Confirm sends always once and closes.
        val closed = open.confirm(question, eligible = true) { sent += it }
        assertFalse(closed.open)
        assertEquals(1, sent.size)
        assertEquals(listOf("always"), sent.single().getJSONObject(0).getJSONArray("selected_option_ids").let { ids -> (0 until ids.length()).map(ids::getString) })
        assertEquals("permission", sent.single().getJSONObject(0).getString("question_id"))
        closed.confirm(question, eligible = true) { sent += it }
        assertEquals(1, sent.size)
        // A card without Always allow cannot open it.
        val narrow = card("opencode_native", JSONArray().put(permission(choices = options("once", "reject"))), approval = true, choices = true)
        assertFalse(AlwaysAllowConfirmation().request(narrow, reviewed = true).open)
    }

    @Test fun alwaysAllowSendsNothingWhenTheCardChangedMeanwhile() {
        val question = card("opencode_native", JSONArray().put(permission()), approval = true, choices = true)
        val open = AlwaysAllowConfirmation().request(question, reviewed = true)
        val changed = mapOf(
            "new hash" to question.copy(hash = "replacement"),
            "new id" to question.copy(id = "per_2"),
            "stopped qualifying" to question.copy(prompts = listOf(question.prompts.single().copy(choices = question.prompts.single().choices + Choice("later", "Later", "")))),
            "always withdrawn" to question.copy(prompts = listOf(question.prompts.single().copy(choices = question.prompts.single().choices.filter { it.id != "always" }))),
            "unanswerable" to question.copy(canAnswer = false))
        changed.forEach { (name, current) ->
            val sent = mutableListOf<JSONArray>()
            assertFalse(name, open.showing(current, eligible = true))
            assertFalse(name, open.confirm(current, eligible = true) { sent += it }.open)
            assertTrue(name, sent.isEmpty())
        }
        // No longer eligible: sending, submitted, the conversation is not verified, or the app left the foreground.
        val sent = mutableListOf<JSONArray>()
        assertFalse(open.showing(question, eligible = false))
        assertFalse(open.confirm(question, eligible = false) { sent += it }.open)
        assertTrue(sent.isEmpty())
    }

    @Test fun permissionButtonsMapToTheirOptionsAndGates() {
        val question = card("opencode_native", JSONArray().put(permission()), approval = true, choices = true)
        val actions = QuestionPolicies.openCodePermissionActions(question)
        assertEquals(listOf(OpenCodePermissionAction.Reject, OpenCodePermissionAction.AllowOnce, OpenCodePermissionAction.AlwaysAllow), actions)
        assertEquals(listOf("reject", "once", "always"), actions.map { it.option })
        assertEquals(listOf("Reject", "Allow once (3)", "Always allow…"), actions.map { it.label(3) })
        assertEquals(listOf("Reject", "Allow once", "Always allow…"), actions.map { it.label(0) })
        // Reject needs no review; Allow once and Always allow wait for it; only Always allow asks again first.
        assertEquals(listOf(false, true, true), actions.map { it.review })
        assertEquals(listOf(false, false, true), actions.map { it.confirmation })
        assertEquals(listOf(true, false, false), actions.map { it.enabled(answerable = true, reviewed = false) })
        assertEquals(listOf(true, true, true), actions.map { it.enabled(answerable = true, reviewed = true) })
        assertEquals(listOf(false, false, false), actions.map { it.enabled(answerable = false, reviewed = true) })
        // Each sends the one row of its own option.
        actions.forEach { action ->
            assertEquals(action.option, QuestionPolicies.approvalAnswer(question, action.option).getJSONObject(0).getJSONArray("selected_option_ids").getString(0))
        }
        // Without always there is no Always allow…; a card that does not qualify has no permission buttons.
        val narrow = card("opencode_native", JSONArray().put(permission(choices = options("once", "reject"))), approval = true, choices = true)
        assertEquals(listOf(OpenCodePermissionAction.Reject, OpenCodePermissionAction.AllowOnce), QuestionPolicies.openCodePermissionActions(narrow))
        assertTrue(QuestionPolicies.openCodePermissionActions(card("claude_tool_approval", JSONArray().put(permission()), approval = true, choices = true)).isEmpty())
        assertTrue(QuestionPolicies.openCodePermissionActions(question.copy(canAnswer = false)).isEmpty())
    }

    @Test fun alwaysAllowConfirmationNamesWhatItSaves() {
        // hgs describes Always allow with the patterns OpenCode stops asking for; the dialog shows it verbatim.
        val saved = "Allows without asking again: printf *"
        val described = JSONArray().put(JSONObject().put("id", "once").put("label", "Allow once"))
            .put(JSONObject().put("id", "always").put("label", "Always allow").put("description", saved).put("scope", "broader"))
            .put(JSONObject().put("id", "reject").put("label", "Reject").put("description", "Rejects every pending request in this session"))
        val question = card("opencode_native", JSONArray().put(permission(choices = described)), approval = true, choices = true)
        assertEquals(saved, QuestionPolicies.alwaysAllowDescription(question))
        val several = "Allows without asking again: git push *, git fetch *"
        val wider = question.copy(prompts = listOf(question.prompts.single().copy(choices = question.prompts.single().choices.map {
            if (it.id == "always") it.copy(description = several) else it })))
        assertEquals(several, QuestionPolicies.alwaysAllowDescription(wider))
        // Older cards without a description show only the fixed text; other cards have none.
        assertEquals("", QuestionPolicies.alwaysAllowDescription(card("opencode_native", JSONArray().put(permission()), approval = true, choices = true)))
        assertEquals("", QuestionPolicies.alwaysAllowDescription(question.copy(canAnswer = false)))
        assertEquals("", QuestionPolicies.alwaysAllowDescription(card("claude_tool_approval", JSONArray().put(permission(choices = described)), approval = true, choices = true)))
    }

    @Test fun approvalRequiresExactNativeIds() {
        val approval = question().copy(approval = true)
        assertFalse(QuestionPolicies.approvalSupported(approval))
        assertTrue(QuestionPolicies.approvalSupported(approval.copy(prompts = listOf(prompt.copy(choices = listOf(Choice("allow", "Yes", ""), Choice("deny", "No", "")))))))
    }

    /** The Kimi Bash approval card hgs builds (src/state/kimi_approval.rs), with its answer transport verified. */
    private fun kimiApprovalCard(edit: (JSONObject) -> Unit = {}): JSONObject {
        val card = JSONObject().put("question_id", "approval_1").put("question_hash", "kimi-hash").put("tool_call_id", "tool_1")
            .put("agent_id", "main").put("run_id", target.run).put("conversation_id", target.conversation).put("created_at", 1.5)
            .put("source", "kimi_tool_approval").put("approval", true).put("approval_choices", true).put("can_answer", true)
            .put("answer_transport", "kimi_tui").put("answer_unavailable_reason", "")
            .put("questions", JSONArray().put(JSONObject().put("id", "approval").put("question", "Run this command?").put("header", "Bash")
                .put("body", "cwd: /synthetic/work\nDangerous: recursive delete\n$ rm -rf .cache").put("multi_select", false)
                .put("allow_other", true).put("other_label", "Reject with feedback")
                .put("other_description", "Reject this request with your message; Kimi continues without it")
                .put("options", JSONArray()
                    .put(JSONObject().put("id", "1").put("label", "Approve once").put("description", "").put("scope", "once"))
                    .put(JSONObject().put("id", "2").put("label", "Approve for this session")
                        .put("description", "Kimi also stops asking about this exact command for the rest of this session.").put("scope", "broader"))
                    .put(JSONObject().put("id", "3").put("label", "Reject").put("description", "").put("scope", "once")))))
        edit(card)
        return card
    }
    private fun parsed(card: JSONObject) = NativeParser.questions(JSONObject().put("pending_questions", JSONArray().put(card))).single()
    private fun item(card: JSONObject) = card.getJSONArray("questions").getJSONObject(0)

    @Test fun nativeApprovalChoicesQualifyInTheirExactShape() {
        val kimi = parsed(kimiApprovalCard())
        assertTrue(kimi.approvalChoices)
        assertEquals(listOf("once", "broader", "once"), kimi.prompts.single().choices.map { it.scope })
        assertTrue(QuestionPolicies.nativeChoices(kimi))
        assertFalse(QuestionPolicies.approvalSupported(kimi))
        assertFalse(QuestionPolicies.openCodePermission(kimi))
        // Kimi's Reject with feedback is the card's own text answer.
        assertEquals("Reject with feedback", QuestionPolicies.textField(kimi, kimi.prompts.single())?.label)
        // A Claude terminal panel: its options verbatim, without a text answer.
        val claude = parsed(kimiApprovalCard {
            it.put("source", "claude_tool_approval").put("answer_transport", "claude_tui")
            item(it).put("allow_other", false).remove("other_label")
            item(it).put("options", JSONArray().put(JSONObject().put("id", "1").put("label", "Yes").put("scope", "once"))
                .put(JSONObject().put("id", "2").put("label", "Yes, and don't ask again for git commands in /synthetic/work").put("scope", "broader"))
                .put(JSONObject().put("id", "3").put("label", "No").put("scope", "once")))
        })
        assertTrue(QuestionPolicies.nativeChoices(claude))
        assertNull(QuestionPolicies.textField(claude, claude.prompts.single()))
        // A card that cannot be answered here keeps its native options read-only.
        val readOnly = parsed(kimiApprovalCard { it.put("can_answer", false).put("answer_unavailable_reason", "Open Terminal to answer this approval.") })
        assertTrue(QuestionPolicies.nativeChoices(readOnly))
        assertFalse(QuestionPolicies.inputEnabled(readOnly, true))
        assertNull(QuestionPolicies.textField(readOnly, readOnly.prompts.single()))
        assertEquals("Open Terminal to answer this approval.", QuestionPolicies.unavailableNotice(readOnly))
        val refused = mapOf<String, (JSONObject) -> Unit>(
            "no approval_choices" to { it.put("approval_choices", false) },
            "older host without the flag" to { it.remove("approval_choices") },
            "not an approval" to { it.put("approval", false) },
            "multi" to { item(it).put("multi_select", true) },
            "no options" to { item(it).put("options", JSONArray()) },
            "missing scope" to { item(it).getJSONArray("options").getJSONObject(0).remove("scope") },
            "unknown scope" to { item(it).getJSONArray("options").getJSONObject(1).put("scope", "always") },
            "repeated id" to { item(it).getJSONArray("options").getJSONObject(2).put("id", "1") },
            "blank id" to { item(it).getJSONArray("options").getJSONObject(2).put("id", "") },
            "second item" to { it.getJSONArray("questions").put(JSONObject(item(it).toString()).put("id", "other")) },
            "OpenCode" to { it.put("source", "opencode_native") })
        refused.forEach { (name, edit) ->
            val question = parsed(kimiApprovalCard(edit))
            assertFalse(name, QuestionPolicies.nativeChoices(question))
            assertNull(name, QuestionPolicies.broaderChoice(question, mapOf("approval" to setOf("2"))))
            if (question.approval) assertNull(name, QuestionPolicies.textField(question, question.prompts.first()))
        }
    }

    @Test fun nativeChoiceAnswersSendTheChosenOptionOrTheFeedbackText() {
        val kimi = parsed(kimiApprovalCard())
        val prompts = kimi.prompts
        fun row(answers: JSONArray): Triple<String, List<String>, String> {
            assertEquals(1, answers.length())
            val row = answers.getJSONObject(0)
            val ids = row.getJSONArray("selected_option_ids")
            return Triple(row.getString("question_id"), (0 until ids.length()).map { ids.getString(it) }, row.getString("text"))
        }
        val once = QuestionPolicies.answers(prompts, mapOf("approval" to setOf("1")), emptyMap())
        assertEquals(Triple("approval", listOf("1"), ""), row(once))
        assertTrue(QuestionPolicies.complete(prompts, mapOf("approval" to setOf("3")), emptyMap()))
        val feedback = mapOf("approval" to "Use rg instead")
        assertEquals(Triple("approval", emptyList<String>(), "Use rg instead"), row(QuestionPolicies.answers(prompts, emptyMap(), feedback)))
        assertTrue(QuestionPolicies.complete(prompts, emptyMap(), feedback))
        assertEquals("Choose one answer for this question.", QuestionPolicies.answerError(prompts, mapOf("approval" to setOf("3")), feedback))
        assertEquals("Enter command-like answers on your computer.", QuestionPolicies.answerError(prompts, emptyMap(), mapOf("approval" to "/rm")))
        assertFalse(QuestionPolicies.complete(prompts, emptyMap(), mapOf("approval" to "line\nbreak")))
        assertFalse(QuestionPolicies.complete(prompts, emptyMap(), emptyMap()))
        assertTrue(QuestionSendPolicies.available(kimi, target, once.toString()))
    }

    @Test fun broaderChoiceAsksForItsOwnConfirmationOfTheExactCardAndOption() {
        val kimi = parsed(kimiApprovalCard())
        val session = mapOf("approval" to setOf("2"))
        val once = mapOf("approval" to setOf("1"))
        assertNull(QuestionPolicies.broaderChoice(kimi, once))
        assertEquals("Approve for this session", QuestionPolicies.broaderChoice(kimi, session)?.label)
        val closed = BroaderChoiceConfirmation()
        assertFalse(closed.request(kimi, session, reviewed = false).open)
        assertFalse(closed.request(kimi, once, reviewed = true).open)
        val open = closed.request(kimi, session, reviewed = true)
        assertEquals(BroaderChoiceConfirmation(kimi.id, kimi.hash, "2"), open)
        assertEquals("2", open.showing(kimi, session, eligible = true)?.id)
        // The dialog closes without sending when anything it confirmed changed.
        assertNull(open.showing(kimi, session, eligible = false))
        assertNull(open.showing(kimi, once, eligible = true))
        assertNull(open.showing(kimi.copy(hash = "changed"), session, eligible = true))
        assertNull(open.showing(parsed(kimiApprovalCard { item(it).getJSONArray("options").getJSONObject(1).put("scope", "once") }), session, eligible = true))
        var sent = 0
        assertFalse(open.confirm(kimi, once, eligible = true) { sent++ }.open)
        assertEquals(0, sent)
        assertFalse(open.confirm(kimi, session, eligible = true) { sent++ }.open)
        assertEquals(1, sent)
        assertFalse(closed.confirm(kimi, session, eligible = true) { sent++ }.open)
        assertEquals(1, sent)
    }
}
