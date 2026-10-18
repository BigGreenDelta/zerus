package app.zerus.mobile

import org.junit.Assert.*
import org.junit.Test

class QuestionLiteralPolicyTest {
    private val question=Question("fixture-question","fixture-hash",emptyList(),false,false,false,"terminal",run="fixture-run",conversation="fixture-conversation")
    @Test fun agentAuthoredDisclosuresKeepMarkdown() {
        for(source in listOf("hook","codex_async","kimi_wire","kimi_plan_review","dsh_native","opencode_native","claude_native_question"))
            assertTrue(source,QuestionPolicies.authoredMarkdown(question.copy(source=source)))
    }
    @Test fun approvalOrTrustAlwaysStaysLiteralIncludingAuthoredSources() {
        for(source in listOf("hook","codex_async","kimi_wire","kimi_plan_review","dsh_native","opencode_native","claude_native_question","claude_terminal_choice")) {
            assertFalse(source,QuestionPolicies.authoredMarkdown(question.copy(source=source,approval=true)))
            assertFalse(source,QuestionPolicies.authoredMarkdown(question.copy(source=source,trustRequest=true)))
            assertFalse(source,QuestionPolicies.authoredMarkdown(question.copy(source=source,approval=true,trustRequest=true)))
        }
    }
    @Test fun terminalAndUnknownSourcesPreserveRenderedPathsAndChoices() {
        for(source in listOf("claude_folder_trust","codex_folder_trust","kimi_folder_trust","claude_resume","codex_update",
            "claude_terminal_choice","claude_tool_approval","claude_terminal_safety","startup_terminal_choice","terminal","new_native_source","HOOK",""))
            assertFalse(source,QuestionPolicies.authoredMarkdown(question.copy(source=source)))
    }
}
