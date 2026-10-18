package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

/** OpenCode variants as launch and model-sheet effort choices (synthetic catalog values). */
class OpenCodeEffortTest {
    private val target = Target("computer","opencode/project/local","run","ses_fixture","workspace")
    private fun option(value:String,label:String,efforts:List<String>,labels:List<String> = efforts) = JSONObject().put("id",value).put("value",value)
        .put("label",label).put("provider",value.substringBefore('/')).put("provider_label","Fixture").put("free",true)
        .put("effort_options",JSONArray(efforts)).put("effort_labels",JSONArray(labels))
    private fun catalog() = JSONObject().put("agents",JSONArray(listOf("codex","opencode")))
        .put("accounts",JSONArray().put(JSONObject().put("id","native-opencode").put("provider","opencode").put("label","Native configuration")
            .put("native",true).put("installed",true).put("is_default",true).put("catalog_state","ready")
            .put("model_options",JSONArray().put(option("fixture/local","Local",listOf("low","high","thinking"),listOf("Low","High","Deep thinking")))
                .put(option("fixture/plain","Plain",emptyList()))
                .put(option("fixture/odd","Odd",listOf("ok","has space","with/slash","x".repeat(65),"ok"),listOf("Fine")))))
            .put(JSONObject().put("id","default").put("provider","codex").put("label","Default account")))
    private fun launch(raw:JSONObject,model:String,effort:String,supported:Boolean,agent:String="opencode",account:String="native-opencode") =
        LaunchPresentation.arguments(raw,agent,"/fixture/project","local",account,"",null,model,effort,supported)

    @Test fun launchOffersTheChosenModelsVariantsOnlyWhenTheConnectorAcceptsThem() {
        val raw = LaunchPresentation.sanitizeCatalog(catalog())
        assertEquals(listOf(LaunchEffort("low","Low"),LaunchEffort("high","High"),LaunchEffort("thinking","Deep thinking")),
            LaunchPresentation.efforts(raw,"opencode","native-opencode","fixture/local",true))
        // No row: an old connector, a model without variants, the native default model, another agent.
        assertTrue(LaunchPresentation.efforts(raw,"opencode","native-opencode","fixture/local",false).isEmpty())
        assertTrue(LaunchPresentation.efforts(raw,"opencode","native-opencode","fixture/plain",true).isEmpty())
        assertTrue(LaunchPresentation.efforts(raw,"opencode","native-opencode","",true).isEmpty())
        assertTrue(LaunchPresentation.efforts(raw,"codex","default","fixture/local",true).isEmpty())
        // Unsafe or oversized variant IDs are never offered; a missing label falls back to the ID.
        assertEquals(listOf(LaunchEffort("ok","Fine")),LaunchPresentation.efforts(raw,"opencode","native-opencode","fixture/odd",true))
    }

    @Test fun launchRequestCarriesEffortOnlyWhenOfferedAndAdvertised() {
        val raw = LaunchPresentation.sanitizeCatalog(catalog())
        val chosen = launch(raw,"fixture/local","thinking",true)
        assertEquals("thinking",chosen.getString("effort"));assertEquals("fixture/local",chosen.getString("model"))
        assertFalse(launch(raw,"fixture/local","",true).has("effort"))
        // An old connector never receives effort, even when a stale form still holds one.
        assertThrows(IllegalStateException::class.java) { launch(raw,"fixture/local","thinking",false) }
        assertFalse(launch(raw,"fixture/local","",false).has("effort"))
        // Effort must be one of the chosen model's own variants and requires a model.
        assertThrows(IllegalStateException::class.java) { launch(raw,"fixture/local","max",true) }
        assertThrows(IllegalStateException::class.java) { launch(raw,"fixture/plain","low",true) }
        assertThrows(IllegalStateException::class.java) { launch(raw,"","low",true) }
        assertThrows(IllegalStateException::class.java) { launch(raw,"","low",true,"codex","default") }
        // The durable policy repeats the provider rule for recovered launch receipts.
        val args = JSONObject().put("agent","opencode").put("directory","/fixture/project").put("tag","local").put("account_id","native-opencode")
            .put("model","fixture/local").put("effort","thinking")
        assertEquals("thinking",SessionActionPolicies.arguments("launch",args).getString("effort"))
        assertThrows(IllegalArgumentException::class.java) { SessionActionPolicies.arguments("launch",JSONObject(args.toString()).put("agent","codex")) }
        assertThrows(IllegalArgumentException::class.java) { SessionActionPolicies.arguments("launch",JSONObject(args.toString()).apply { remove("model") }) }
        assertThrows(IllegalArgumentException::class.java) { SessionActionPolicies.arguments("launch",JSONObject(args.toString()).put("effort","has space")) }
    }

    @Test fun nativeEffortNeedsTheFeatureOnBothRelayAndConnector() {
        val feature = setOf(LaunchPresentation.OPENCODE_LAUNCH_EFFORT,"inspect_agent")
        assertTrue(LaunchPresentation.effortSupported(feature,feature))
        assertFalse(LaunchPresentation.effortSupported(setOf("inspect_agent"),feature))   // older relay
        assertFalse(LaunchPresentation.effortSupported(feature,setOf("inspect_agent")))   // Python or older connector
        assertFalse(LaunchPresentation.effortSupported(emptySet(),emptySet()))
        // With an older relay the row is hidden and Create sends no effort, without an error.
        val raw = LaunchPresentation.sanitizeCatalog(catalog())
        val supported = LaunchPresentation.effortSupported(setOf("inspect_agent"),feature)
        assertTrue(LaunchPresentation.efforts(raw,"opencode","native-opencode","fixture/local",supported).isEmpty())
        assertFalse(launch(raw,"fixture/local","",supported).has("effort"))
    }

    @Test fun modelSheetOffersOnlyRelayNamesUntilTheRelayAcceptsNativeVariants() {
        val raw = JSONObject().put("settings_model","fixture/local").put("effort","high").put("settings_change_supported",true)
            .put("model_options",JSONArray().put(option("fixture/local","Local",listOf("low","high","thinking"),listOf("Low","High","Deep thinking"))))
        val settings = SessionDetailsPresentation.settings(raw)
        assertEquals(listOf("","low","high"),SessionDetailsPresentation.effortChoices(settings,"fixture/local",false))
        assertEquals(listOf("","low","high","thinking"),SessionDetailsPresentation.effortChoices(settings,"fixture/local",true))
        // The send path refuses a native-only name without both features, before anything is recorded.
        val thinking = JSONObject().put("model","fixture/local").put("effort","thinking")
        assertEquals("Update this gateway and computer to choose this effort.",SessionActionPolicies.effortReason("settings",thinking,false))
        assertEquals("",SessionActionPolicies.effortReason("settings",thinking,true))
        assertEquals("",SessionActionPolicies.effortReason("settings",JSONObject().put("effort","high"),false))
        assertEquals("",SessionActionPolicies.effortReason("settings",JSONObject().put("model","fixture/local"),false))
        assertEquals("",SessionActionPolicies.effortReason("launch",thinking,false))
    }

    @Test fun launchAndModelSheetReadVariantsWithTheSameRules() {
        val many = (1..20).map { "v$it" }
        val messy = option("fixture/messy","Messy",listOf("low","has space","with/slash","x".repeat(65),"low") + many,listOf("Low","Bad","Bad","Bad","Again") + many.map { it.uppercase() })
        val raw = JSONObject().put("settings_model","fixture/messy").put("settings_change_supported",true)
            .put("effort_options",JSONArray(listOf("low","two words","high"))).put("model_options",JSONArray().put(messy))
        val launch = LaunchPresentation.models(JSONObject().put("accounts",JSONArray().put(JSONObject().put("id","native-opencode").put("provider","opencode")
            .put("model_options",JSONArray().put(messy)))),"opencode","native-opencode").single().efforts
        val settings = SessionDetailsPresentation.settings(raw)
        assertEquals(16,launch.size)
        assertEquals(listOf("low") + many.take(15),launch.map { it.value })
        assertEquals(launch.map { it.value },settings.models.single().efforts)
        assertEquals(launch.map { it.label },settings.models.single().efforts.map { SessionDetailsPresentation.effortLabel(settings,"fixture/messy",it) })
        // The session-wide list follows the same validation.
        assertEquals(listOf("low","high"),settings.efforts)
        // Without reported labels the shared names stay, as for Codex and Claude.
        val codex = SessionDetailsPresentation.settings(JSONObject().put("model_options",JSONArray().put(JSONObject().put("id","gpt").put("effort_options",JSONArray(listOf("xhigh","low"))))))
        assertEquals(listOf("Extra high","Low"),codex.models.single().efforts.map { SessionDetailsPresentation.effortLabel(codex,"gpt",it) })
    }

    @Test fun modelSheetOffersTheSessionModelsOwnVariantsWithNativeLabels() {
        // OpenCode inspection: per-model variants; a fixed session list must not leak into the sheet.
        val raw = JSONObject().put("settings_model","fixture/local").put("model","local").put("effort","high").put("settings_change_supported",true)
            .put("effort_options",JSONArray(listOf("low","medium","high","xhigh")))
            .put("model_options",JSONArray().put(option("fixture/local","Local",listOf("low","high","thinking"),listOf("Low","High","Deep thinking")))
                .put(option("fixture/plain","Plain",emptyList())))
        val settings = SessionDetailsPresentation.settings(raw)
        assertEquals(listOf("","low","high","thinking"),SessionDetailsPresentation.effortChoices(settings,"fixture/local",true))
        assertEquals(listOf("Default","Low","High","Deep thinking"),
            SessionDetailsPresentation.effortChoices(settings,"fixture/local",true).map { SessionDetailsPresentation.effortLabel(settings,"fixture/local",it) })
        assertEquals(listOf(""),SessionDetailsPresentation.effortChoices(settings,"fixture/plain",true))
        // Labels change with the model, so another model's label is never shown.
        assertNotEquals(SessionDetailsPresentation.settings(raw).signature,
            SessionDetailsPresentation.settings(JSONObject(raw.toString()).put("model_options",JSONArray().put(option("fixture/local","Local",listOf("low","high","thinking"))))).signature)
        // Other providers keep the shared names.
        assertEquals("Extra high",SessionDetailsPresentation.effortLabel(settings,"other","xhigh"))
        // A native variant name outside the fixed list is a valid request for the inspected model only.
        val args = SessionActionPolicies.arguments("settings",JSONObject().put("model","fixture/local").put("effort","thinking"))
        assertEquals("",SessionActionPolicies.inspectReason(target,"settings",args,raw))
        assertEquals("The selected effort is unavailable for this model.",
            SessionActionPolicies.inspectReason(target,"settings",JSONObject().put("model","fixture/local").put("effort","xhigh"),raw))
        assertEquals("The selected effort is unavailable for this model.",
            SessionActionPolicies.inspectReason(target,"settings",JSONObject().put("model","fixture/plain").put("effort","thinking"),raw))
        assertThrows(IllegalArgumentException::class.java) { SessionActionPolicies.arguments("settings",JSONObject().put("effort","two words")) }
    }
}
