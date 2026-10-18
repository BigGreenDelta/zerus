package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File

/** Question cards built by hgs from OpenCode forms, shared with iOS and hgs through opencode-form-contract.json. */
class OpenCodeFormContractTest {
    private val contract = JSONObject(checkNotNull(javaClass.getResourceAsStream("/opencode-form-contract.json")).bufferedReader().use { it.readText() })
    private val cases = contract.getJSONArray("cases").objects()
    private fun case(name: String) = cases.single { it.getString("name") == name }
    /**
     * The contract pins the first item; unanswerable cards list only their count. With [pad] the rest are
     * synthetic placeholders; without it the card carries exactly the contract's items.
     */
    private fun question(example: JSONObject, pad: Boolean = true): Question {
        val card = JSONObject(example.getJSONObject("card").toString())
        val items = JSONArray()
        (card.remove("item") as? JSONObject)?.let(items::put)
        while (pad && items.length() < card.getInt("question_count")) items.put(JSONObject().put("id", "field-${items.length()}").put("prompt", "Field ${items.length()}"))
        card.put("questions", items).put("question_hash", "hash-" + example.getString("name"))
        return NativeParser.questions(JSONObject().put("pending_questions", JSONArray().put(card))).single()
    }
    private fun answers(example: JSONObject) = example.getJSONArray("answers").objects().let { rows ->
        rows.associate { it.getString("question_id") to it.getJSONArray("selected_option_ids").let { ids -> (0 until ids.length()).map(ids::getString).toSet() } } to
            rows.associate { it.getString("question_id") to it.getString("text") }
    }

    @Test fun everyContractCardKeepsItsIdentityItemsAndAnswerability() {
        assertEquals(17, cases.size)
        cases.forEach { example ->
            val name = example.getString("name"); val card = example.getJSONObject("card")
            val question = question(example)
            assertEquals(name, card.getString("question_id"), question.id)
            assertEquals(name, card.getBoolean("can_answer"), question.canAnswer)
            assertEquals(name, card.getBoolean("approval"), question.approval)
            // Only the contract's own items: a pinned item is the card's single item; a card
            // without one is unanswerable and still shown with its reason.
            val exact = question(example, pad = false)
            if (card.has("item")) {
                assertEquals(name, 1, card.getInt("question_count"))
                assertEquals(name, listOf(card.getJSONObject("item").getString("id")), exact.prompts.map { it.id })
            } else {
                assertTrue(name, exact.prompts.isEmpty())
                assertFalse(name, card.getBoolean("can_answer"))
                assertTrue(name, QuestionPolicies.shown(exact))
                assertEquals(name, "Answer this form in OpenCode", QuestionPolicies.unavailableNotice(exact))
            }
            assertEquals(name, card.getBoolean("can_answer"), QuestionPolicies.submitVisible(question))
            assertEquals(name, card.optString("unavailable_reason"), question.unavailableReason)
            val item = card.optJSONObject("item") ?: return@forEach
            val prompt = question.prompts.first()
            assertEquals(name, item.getString("id"), prompt.id)
            assertEquals(name, item.getString("prompt"), prompt.text)
            assertEquals(name, item.getString("header"), prompt.header)
            assertEquals(name, item.getBoolean("multi_select"), prompt.multi)
            assertEquals(name, item.getBoolean("allow_other"), prompt.other)
            assertEquals(name, item.optBoolean("required", true), prompt.required)
            assertEquals(name, item.getJSONArray("options").objects().map { it.getString("id") to it.getString("label") }, prompt.choices.map { it.id to it.label })
            assertEquals(name, item.optString("input_kind"), prompt.inputKind)
            assertEquals(name, item.optInt("max_length", -1), prompt.maxLength ?: -1)
            assertEquals(name, item.optInt("max_items", -1), prompt.maxItems ?: -1)
            assertEquals(name, item.optJSONArray("default_option_ids")?.let { ids -> (0 until ids.length()).map(ids::getString) }.orEmpty(), prompt.defaults)
        }
    }

    @Test fun plainStringShowsATextFieldLimitedToFiveCharacters() {
        val question = question(case("plain-string"))
        val prompt = question.prompts.single()
        val field = checkNotNull(QuestionPolicies.textField(question, prompt))
        assertEquals("Answer", field.label)
        assertEquals(5, field.maxLength)
        assertEquals("toolo", QuestionPolicies.limitText("toolong", field.maxLength))
        assertEquals("toolong", QuestionPolicies.limitText("toolong", null))
        // Five astral characters are five characters, as hgs counts them.
        assertEquals("😀".repeat(5), QuestionPolicies.limitText("😀".repeat(6), 5))
        assertEquals("Answer is longer than 5 characters",
            QuestionPolicies.answerError(question.prompts, emptyMap(), mapOf(prompt.id to "toolong")))
        assertTrue(QuestionPolicies.complete(question.prompts, emptyMap(), mapOf(prompt.id to "Ada")))
        assertFalse(QuestionPolicies.complete(question.prompts, emptyMap(), mapOf(prompt.id to "")))
        // A choice with a custom answer keeps the familiar label.
        assertEquals("Other answer", QuestionPolicies.textField(question(case("question-single")), question(case("question-single")).prompts.single())?.label)
        assertNull(QuestionPolicies.textField(question(case("question-custom-forbidden")), question(case("question-custom-forbidden")).prompts.single()))
    }

    @Test fun anUnanswerableCardIsReadOnly() {
        val answerable = question(case("question-single"))
        val locked = answerable.copy(canAnswer = false, unavailableReason = "Answer this form in OpenCode")
        assertNotNull(QuestionPolicies.textField(answerable, answerable.prompts.single()))
        assertNull(QuestionPolicies.textField(locked, locked.prompts.single()))
        assertTrue(QuestionPolicies.inputEnabled(answerable, true))
        assertFalse(QuestionPolicies.inputEnabled(locked, true))
        assertFalse(QuestionPolicies.inputEnabled(answerable, false))
        assertFalse(QuestionPolicies.submitVisible(locked))
    }

    @Test fun anOptionalFormItemMayBeLeftEmpty() {
        // hgs omits an empty answer to a field that is not required; the answer row keeps its usual shape.
        val required = question(case("plain-string")).prompts.single()
        val optional = required.copy(required = false)
        assertFalse(QuestionPolicies.complete(listOf(required), emptyMap(), emptyMap()))
        assertTrue(QuestionPolicies.complete(listOf(optional), emptyMap(), emptyMap()))
        assertTrue(QuestionSteps.current(listOf(optional), 0, emptyMap(), emptyMap()).complete)
        val row = QuestionPolicies.answers(listOf(optional), emptyMap(), emptyMap()).getJSONObject(0)
        assertEquals("k", row.getString("question_id")); assertEquals(0, row.getJSONArray("selected_option_ids").length()); assertEquals("", row.getString("text"))
        // Bounds still apply to a typed answer.
        assertEquals("Answer is longer than 5 characters", QuestionPolicies.answerError(listOf(optional), emptyMap(), mapOf("k" to "toolong")))
        // Items without the form fields (Codex optional questions) still need an answer or Skip.
        val legacy = Prompt("q_0", "Any notes?", listOf(Choice("0", "One", "")), false, false, true)
        assertFalse(QuestionPolicies.complete(listOf(legacy), emptyMap(), emptyMap()))
    }

    /** The plain-string card as hgs emits it for a string field with a string default. */
    private fun withDefault(default: Any, hash: String = "hash-default", source: String = "opencode_native", item: JSONObject = case("plain-string").getJSONObject("card").getJSONObject("item")): Question {
        val card = JSONObject(case("plain-string").getJSONObject("card").toString()).put("question_hash", hash).put("source", source)
        card.remove("item")
        card.put("questions", JSONArray().put(JSONObject(item.toString()).put("default_text", default)))
        return NativeParser.questions(JSONObject().put("pending_questions", JSONArray().put(card))).single()
    }

    @Test fun anOpenCodeTextDefaultIsPrefilledUnlessADraftExists() {
        val target = Target("computer", "opencode/project/local", "run", "ses_A", "workspace")
        val question = withDefault("Ada")
        assertEquals("Ada", question.prompts.single().defaultText)
        val prefilled = QuestionPolicies.initialTexts(question.prompts, emptyMap())
        assertEquals(mapOf("k" to "Ada"), prefilled)
        assertTrue(QuestionPolicies.complete(question.prompts, emptyMap(), prefilled))
        // A saved draft for this exact question wins, even when its text is intentionally empty.
        val draft = Draft(target, "", questionId = question.id, questionHash = question.hash,
            answers = QuestionPolicies.answers(question.prompts, emptyMap(), mapOf("k" to "")).toString())
        val saved = QuestionPolicies.savedAnswers(checkNotNull(QuestionPolicies.savedDraft(listOf(draft), target, question)).answers)
        assertEquals(mapOf("k" to ""), QuestionPolicies.initialTexts(question.prompts, saved))
        // A changed question (another hash or ID) has no saved draft and gets the default again.
        listOf(withDefault("Ada", hash = "hash-changed"), question.copy(id = "frm_2")).forEach { changed ->
            assertNull(QuestionPolicies.savedDraft(listOf(draft), target, changed))
            assertEquals(mapOf("k" to "Ada"), QuestionPolicies.initialTexts(changed.prompts, emptyMap()))
        }
        // Bounded like typed text: clipped to max_length; control characters or non-strings are ignored.
        assertEquals("Adela", withDefault("Adelaide").prompts.single().defaultText)
        assertEquals("", withDefault("A\nB").prompts.single().defaultText)
        assertEquals("", withDefault("A\u0007").prompts.single().defaultText)
        assertEquals("", withDefault(42).prompts.single().defaultText)
    }

    @Test fun defaultTextNeedsAnOpenCodeTextItem() {
        // Legacy cards without default_text are unchanged.
        val legacy = question(case("plain-string"))
        assertEquals("", legacy.prompts.single().defaultText)
        assertEquals(mapOf("k" to ""), QuestionPolicies.initialTexts(legacy.prompts, emptyMap()))
        // A choice item or a non-OpenCode card ignores it.
        val choice = withDefault("Purple", item = case("question-single").getJSONObject("card").getJSONObject("item"))
        assertEquals("", choice.prompts.single().defaultText)
        assertEquals("", withDefault("Ada", source = "codex_async").prompts.single().defaultText)
    }

    @Test fun booleanPreselectsTheNativeDefault() {
        val question = question(case("boolean"))
        val options = QuestionPolicies.initialOptions(question.prompts, emptyMap())
        assertEquals(mapOf("k" to setOf("yes")), options)
        assertTrue(QuestionPolicies.complete(question.prompts, options, emptyMap()))
        assertNull(QuestionPolicies.textField(question, question.prompts.single()))
        // A saved answer wins over the default, and unknown defaults are ignored.
        assertEquals(mapOf("k" to setOf("no")), QuestionPolicies.initialOptions(question.prompts, mapOf("k" to NativeQuestionAnswer("k", listOf("no"), ""))))
        val stale = question.prompts.single().copy(defaults = listOf("maybe"))
        assertEquals(mapOf("k" to emptySet<String>()), QuestionPolicies.initialOptions(listOf(stale), emptyMap()))
    }

    @Test fun unsupportedNumberShowsTheReasonWithoutSubmit() {
        val question = question(case("unsupported-number"))
        assertFalse(question.canAnswer)
        assertFalse(QuestionPolicies.submitVisible(question))
        assertEquals("Answer this form in OpenCode", question.unavailableReason)
        assertEquals("Answer this form in OpenCode", QuestionPolicies.unavailableNotice(question))
        // Older cards keep their reason field and the generic fallback.
        val legacy = NativeParser.questions(JSONObject().put("pending_questions", JSONArray().put(JSONObject().put("question_id", "q")
            .put("can_answer", false).put("answer_unavailable_reason", "Answer this in Terminal.")
            .put("questions", JSONArray().put(JSONObject().put("id", "a").put("question", "Continue?")))))).single()
        assertEquals("Answer this in Terminal.", QuestionPolicies.unavailableNotice(legacy))
        assertEquals("Complete this request on your machine.", QuestionPolicies.unavailableNotice(legacy.copy(unavailableReason = "")))
    }

    @Test fun anUnanswerableCardWithoutItemsStillShowsItsReason() {
        // hgs clears the items of a form over 16 KiB; the contract's oversized card without them.
        val card = JSONObject(case("oversized").getJSONObject("card").toString()).put("questions", JSONArray()).put("question_hash", "hash-cleared")
        val cleared = NativeParser.questions(JSONObject().put("pending_questions", JSONArray().put(card))).single()
        assertTrue(cleared.prompts.isEmpty())
        assertTrue(QuestionPolicies.shown(cleared))
        val target = Target("computer", "opencode/project/local", "run", "ses_A", "workspace")
        assertEquals(cleared, QuestionPolicies.queue(listOf(cleared), target, "").selected)
        assertFalse(QuestionPolicies.submitVisible(cleared))
        assertEquals("Answer this form in OpenCode", QuestionPolicies.unavailableNotice(cleared))
        val step = QuestionSteps.current(cleared.prompts, 0, emptyMap(), emptyMap())
        assertNull(step.nextPage); assertFalse(step.complete)
        assertFalse(QuestionPolicies.complete(cleared.prompts, emptyMap(), emptyMap()))
        // An answerable card still needs items, and an unnamed card is never shown.
        assertFalse(QuestionPolicies.shown(cleared.copy(canAnswer = true)))
        assertNull(QuestionPolicies.queue(listOf(cleared.copy(canAnswer = true)), target, "").selected)
        assertFalse(QuestionPolicies.shown(cleared.copy(id = "")))
    }

    @Test fun multiselectBoundsMatchTheComputerValidation() {
        val question = question(case("multiselect"))
        val (chosen, texts) = answers(case("multiselect"))
        assertEquals("", QuestionPolicies.answerError(question.prompts, chosen, texts))
        assertTrue(QuestionPolicies.complete(question.prompts, chosen, texts))
        val (tooMany, more) = answers(case("multiselect-too-many"))
        assertEquals(case("multiselect-too-many").getString("error"), QuestionPolicies.answerError(question.prompts, tooMany, more))
        assertFalse(QuestionPolicies.complete(question.prompts, tooMany, more))
        val least = question.prompts.single().copy(minItems = 2)
        assertEquals("Choose at least 2 answers", QuestionPolicies.answerError(listOf(least), mapOf("k" to setOf("a")), emptyMap()))
        assertFalse(QuestionPolicies.complete(listOf(least), mapOf("k" to setOf("a")), emptyMap()))
        assertEquals("", QuestionPolicies.answerError(listOf(least), mapOf("k" to emptySet()), emptyMap()))
        val short = question(case("plain-string")).prompts.single().copy(minLength = 3)
        assertEquals("Answer is shorter than 3 characters", QuestionPolicies.answerError(listOf(short), emptyMap(), mapOf("k" to "ab")))
    }

    @Test fun contractAnswersPassOrFailLocallyAsOnTheComputer() {
        cases.filter { it.has("answers") && !it.getJSONObject("card").getBoolean("approval") }.forEach { example ->
            val name = example.getString("name")
            val (chosen, texts) = answers(example)
            val question = question(example)
            val error = QuestionPolicies.answerError(question.prompts, chosen, texts, QuestionPolicies.literalText(question))
            if (example.has("reply")) assertEquals(name, "", error) else assertTrue(name, error.isNotBlank())
        }
    }

    /** Answer rows as plain values, so key order cannot matter. */
    private fun rows(answers: JSONArray) = answers.objects().map { row -> row.keys().asSequence().sorted().associateWith { key ->
        row.get(key).let { value -> if (value is JSONArray) (0 until value.length()).map(value::get) else value }
    } }

    @Test fun textStartingWithSlashOrBangIsAnOrdinaryFormAnswer() {
        // hgs sends OpenCode form text through the form reply API, never to a terminal.
        listOf("plain-string-slash", "plain-string-bang").forEach { name ->
            val example = case(name)
            val question = question(example)
            val (chosen, texts) = answers(example)
            val literal = QuestionPolicies.literalText(question)
            assertTrue(name, literal)
            assertEquals(name, "", QuestionPolicies.answerError(question.prompts, chosen, texts, literal))
            assertTrue(name, QuestionPolicies.complete(question.prompts, chosen, texts, literal))
            assertTrue(name, QuestionSteps.current(question.prompts, 0, chosen, texts, literal).complete)
            assertEquals(name, rows(example.getJSONArray("answers")), rows(QuestionPolicies.answers(question.prompts, chosen, texts)))
        }
        // The same holds for an Other answer on an OpenCode choice item.
        val choice = question(case("question-custom-text"))
        listOf("/tmp/report.txt", "!important").forEach { text ->
            assertEquals(text, "", QuestionPolicies.answerError(choice.prompts, emptyMap(), mapOf("q0" to text), QuestionPolicies.literalText(choice)))
            assertTrue(text, QuestionPolicies.complete(choice.prompts, emptyMap(), mapOf("q0" to text), QuestionPolicies.literalText(choice)))
        }
        // A caller that does not say the card is a form keeps the refusal.
        assertEquals("Enter command-like answers on your computer.", QuestionPolicies.answerError(choice.prompts, emptyMap(), mapOf("q0" to "/tmp/report.txt")))
    }

    @Test fun anOptionalTextFieldCompletesEmpty() {
        val example = case("plain-string-optional")
        val question = question(example)
        assertFalse(question.prompts.single().required)
        assertTrue(QuestionPolicies.complete(question.prompts, emptyMap(), emptyMap(), QuestionPolicies.literalText(question)))
        assertTrue(QuestionSteps.current(question.prompts, 0, emptyMap(), emptyMap(), QuestionPolicies.literalText(question)).complete)
        assertEquals(rows(example.getJSONArray("answers")), rows(QuestionPolicies.answers(question.prompts, emptyMap(), emptyMap())))
    }

    @Test fun everyPermissionCardCanBeAnsweredOnThePhone() {
        val permissions = cases.filter { it.getJSONObject("card").getString("kind") == "opencode_permission" }
        assertEquals(listOf("permission", "permission-once", "permission-reject", "permission-allow-refused"), permissions.map { it.getString("name") })
        permissions.forEach { example ->
            val name = example.getString("name"); val question = question(example)
            assertTrue(name, example.getJSONObject("card").getBoolean("approval_choices"))
            assertTrue(name, QuestionPolicies.openCodePermission(question))
            assertFalse(name, QuestionPolicies.literalText(question))
            assertTrue(name, QuestionPolicies.alwaysAllowOffered(question))
            // Allow/deny approvals keep their own rule; it is not widened.
            assertFalse(name, QuestionPolicies.approvalSupported(question))
        }
    }

    @Test fun permissionButtonsSendTheContractAnswers() {
        val question = question(case("permission"))
        // Reject, Allow once and the confirmed Always allow send exactly the rows hgs accepts.
        mapOf("once" to "permission-once", "always" to "permission", "reject" to "permission-reject").forEach { (option, name) ->
            assertEquals(name, rows(case(name).getJSONArray("answers")), rows(QuestionPolicies.approvalAnswer(question, option)))
        }
        var sent: JSONArray? = null
        AlwaysAllowConfirmation().request(question, reviewed = true).confirm(question, editable = true) { sent = it }
        assertEquals(rows(case("permission").getJSONArray("answers")), rows(checkNotNull(sent)))
        // hgs refuses allow/deny for these cards; no phone control offers them.
        val (refused, texts) = answers(case("permission-allow-refused"))
        assertEquals("Unknown answer option", case("permission-allow-refused").getString("error"))
        assertTrue(QuestionPolicies.answerError(question.prompts, refused, texts).isNotBlank())
        assertTrue(question.prompts.single().choices.none { it.id == "allow" || it.id == "deny" })
    }

    @Test fun androidCopyMatchesTheSharedFixture() {
        val shared = generateSequence(File("").absoluteFile) { it.parentFile }
            .map { File(it, "tests/fixtures/opencode-form-contract.json") }.firstOrNull { it.isFile }
        assumeTrue("The shared fixture is outside this checkout", shared != null)
        val copy = checkNotNull(javaClass.getResourceAsStream("/opencode-form-contract.json")).use { it.readBytes() }
        assertArrayEquals(shared!!.readBytes(), copy)
    }
}
