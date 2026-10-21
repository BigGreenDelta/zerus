package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class OpenCodePresentationTest {
    @Test fun sessionModelSheetReadsMarksAndTruncation() {
        val contract = JSONObject(checkNotNull(javaClass.getResourceAsStream("/opencode-provider-catalog.json")).bufferedReader().use { it.readText() })
        val raw = JSONObject().put("settings_model", "fixture/local").put("settings_change_supported", true)
            .put("model_options", contract.getJSONObject("row").getJSONArray("model_options")).put("model_options_truncated", true)
        val settings = SessionDetailsPresentation.settings(raw)
        assertTrue(settings.modelsTruncated)
        assertEquals("Groq", settings.models.single { it.id == "groq/groq/compound" }.providerLabel)
        assertTrue(settings.models.single { it.id == "groq/groq/compound" }.free)
        assertTrue(settings.models.single { it.id == "keyed/paid-model" }.favorite)
    }

    @Test fun marksAndTruncationInvalidateTheSnapshotButOtherProvidersKeepTheirPlainChoices() {
        val option = JSONObject().put("value", "p/model").put("provider", "p").put("favorite", false)
            .put("effort_options", JSONArray(listOf("native-fast", "high")))
        val raw = JSONObject().put("agent", "opencode").put("model_options", JSONArray().put(option))
        val original = SessionDetailsPresentation.settings(raw)
        assertEquals("p/model", original.models.single().id)
        assertEquals(listOf("", "high"), SessionDetailsPresentation.effortChoices(original, "p/model", false))
        assertEquals(listOf("", "native-fast", "high"), SessionDetailsPresentation.effortChoices(original, "p/model", true))
        option.put("favorite", true)
        assertNotEquals(original.signature, SessionDetailsPresentation.settings(raw).signature)
        option.put("favorite", false); raw.put("model_options_truncated", true)
        assertNotEquals(original.signature, SessionDetailsPresentation.settings(raw).signature)
        raw.put("agent", "codex").put("model_options", JSONArray().put(JSONObject().put("id", "codex-model").put("label", "Codex")))
        val plain = SessionDetailsPresentation.settings(raw)
        assertEquals(listOf("codex-model"), plain.models.map { it.id }); assertEquals("", plain.models.single().provider); assertFalse(plain.modelsTruncated)
    }
    private val target=Target("computer","opencode/project/local","run","native-session","workspace")
    private fun catalog()=JSONObject().put("agents",JSONArray(listOf("codex","opencode","unknown","opencode")))
        .put("accounts",JSONArray().put(JSONObject().put("id","native-opencode").put("provider","opencode").put("label","Native configuration")
            .put("native",true).put("installed",true).put("is_default",true).put("secret","NEVER_RETAIN")
            .put("model_options",JSONArray().put(JSONObject().put("value","ollama/qwen3:8b").put("label","Qwen 3").put("api_key","NEVER_RETAIN"))
                .put(JSONObject().put("id","ollama/qwen3:14b").put("label","Qwen 3 large"))
                .put(JSONObject().put("value","ollama/qwen3:8b").put("label","Duplicate"))
                .put(JSONObject().put("value","invalid\nmodel")))))
    @Test fun exactNativeAccountAndModelsSurviveSanitizationWithoutProviderSecrets() {
        val raw=catalog();val clean=LaunchPresentation.sanitizeCatalog(raw)
        assertEquals(listOf("codex","opencode"),LaunchPresentation.agents(clean))
        val account=LaunchPresentation.accounts(clean,"opencode").single()
        assertEquals("native-opencode",account.id);assertTrue(account.native);assertTrue(account.isDefault)
        assertEquals(listOf(LaunchModel("ollama/qwen3:8b","Qwen 3"),LaunchModel("ollama/qwen3:14b","Qwen 3 large")),LaunchPresentation.models(clean,"opencode",account.id))
        assertTrue(LaunchPresentation.models(clean,"opencode","wrong-account").isEmpty())
        assertTrue(LaunchPresentation.models(clean,"codex",account.id).isEmpty())
        assertFalse(clean.toString().contains("NEVER_RETAIN"))
    }
    @Test fun nativeDefaultDoesNotRequireAReportedModelCatalog() {
        val raw=catalog();raw.getJSONArray("accounts").getJSONObject(0).remove("model_options")
        assertTrue(LaunchPresentation.models(raw,"opencode","native-opencode").isEmpty())
        assertEquals("",LaunchPresentation.disabledReason(LaunchReadiness()))
        val args=JSONObject().put("agent","opencode").put("directory","/fixture/project").put("tag","local").put("account_id","native-opencode")
        assertFalse(SessionActionPolicies.arguments("launch",args).has("model"))
        args.put("model","ollama/qwen3:8b")
        assertEquals("ollama/qwen3:8b",SessionActionPolicies.arguments("launch",args).getString("model"))
        args.put("agent","codex")
        assertThrows(IllegalArgumentException::class.java) { SessionActionPolicies.arguments("launch",args) }
    }
    @Test fun nativeCapabilitiesNeverInventTerminalAttachmentsOrLifecycleControls() {
        val raw=JSONObject().put("terminal_supported",false).put("terminal_reason","OpenCode uses the native API")
            .put("attachments_supported",false).put("attachments_reason","Files are unavailable")
            .put("allowed_actions",JSONArray(listOf("pause","rename","forget")))
            .put("action_reasons",JSONObject().put("archive","Archiving is not supported"))
        assertEquals("OpenCode uses the native API",SessionActionsMenu.nativeTerminalReason(target,raw))
        assertTrue(SessionActionsMenu.nativeTerminalReason(target,JSONObject()).isNotBlank())
        assertEquals("",SessionActionsMenu.nativeTerminalReason(target,JSONObject().put("terminal_supported",true)))
        assertTrue(SessionActionsMenu.nativeTerminalReason(target,JSONObject().put("terminal_supported","true")).isNotBlank())
        assertEquals("Files are unavailable",NativeAttachments.reason(target,raw))
        assertTrue(NativeAttachments.reason(target,null).isNotBlank())
        assertEquals("",SessionControlPresentation.reason(target,"pause",raw))
        assertEquals("Archiving is not supported",SessionControlPresentation.reason(target,"archive",raw))
        assertTrue(SessionControlPresentation.reason(target,"terminate",raw).isNotBlank())
        assertEquals("",NativeAttachments.reason(target.copy(session="codex/project/local"),JSONObject()))
    }
    @Test fun labelAndNativeModelSettingsUseExactProviderModelIdentity() {
        assertEquals("OpenCode",DesktopIcons.providerName("opencode"));assertEquals("OpenCode",DesktopIcons.providerName("Open Code"))
        val raw=JSONObject().put("settings_model","ollama/qwen3:8b").put("model","qwen3:8b").put("settings_change_supported",true)
            .put("model_options",JSONArray().put(JSONObject().put("id","ollama/qwen3:8b").put("label","Qwen 3").put("effort_options",JSONArray())))
        val model=SessionDetailsPresentation.settings(raw)
        assertEquals("ollama/qwen3:8b",model.model);assertEquals("ollama/qwen3:8b",model.models.single().id)
    }
}
