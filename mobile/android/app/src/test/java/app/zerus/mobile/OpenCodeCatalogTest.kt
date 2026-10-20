package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

/** OpenCode providers phase 1, shared with hgs, the connector, the desktop and iOS (opencode-provider-catalog.json). */
class OpenCodeCatalogTest {
    @Test fun capsKeepTheFirstDistinctValidModelsAndProviders() {
        val models = JSONArray().put(JSONObject().put("value", "invalid"))
            .put(JSONObject().put("value", "p/m0").put("label", "First 0"))
            .put(JSONObject().put("value", "p/m0").put("label", "Replacement"))
        repeat(300) { i -> models.put(JSONObject().put("value", "p/m$i").put("label", "First $i")) }
        models.put(JSONObject().put("value", "p/m0").put("label", "Replacement"))
        val parsed = OpenCodeCatalog.models(models)
        assertEquals(256, parsed.size); assertEquals("p/m255", parsed.last().value); assertEquals("First 0", parsed.first().label)
        val providers = JSONArray().put(JSONObject().put("id", "bad id"))
        repeat(70) { providers.put(JSONObject().put("id", "p$it")) }
        assertEquals(64, OpenCodeCatalog.providers(providers).size); assertEquals(16, OpenCodeCatalog.providers(providers, 16).size)
        assertTrue(OpenCodeCatalog.providers(providers, -1).isEmpty())
    }

    @Test fun typedMetadataRejectsCoercionAndRetainsNativeEffortMembership() {
        val row = JSONObject().put("value", "p/model").put("provider", "bad id").put("provider_label", "\u0000")
            .put("label", true).put("free", 1).put("favorite", "true").put("default", false)
            .put("effort_options", JSONArray(listOf("native-fast", "bad value", "native-fast", "other")))
            .put("effort_labels", JSONArray(listOf("Native fast", "Bad", "Duplicate", "Other")))
        val model = OpenCodeCatalog.models(JSONArray().put(row)).single()
        assertEquals("p/model", model.label); assertEquals("", model.provider); assertEquals("", model.providerLabel)
        assertTrue(OpenCodeCatalog.marks(model).isEmpty()); assertEquals(listOf("native-fast", "other"), model.efforts.map { it.value })
        val providers = OpenCodeCatalog.providers(JSONArray().put(JSONObject().put("id", "p")
            .put("credential_count", "2").put("model_count", 1.5).put("active_label", "x".repeat(81))))
        assertNull(providers.single().credentials); assertNull(providers.single().models); assertEquals("", providers.single().activeLabel)
        val account = OpenCodeCatalog.account(JSONObject().put("provider", "opencode").put("catalog_at", true)
            .put("providers_truncated", "true").put("model_options_truncated", 1))!!
        assertNull(account.catalogAt); assertFalse(account.providersTruncated); assertFalse(account.modelsTruncated)
    }
    private val contract = JSONObject(checkNotNull(javaClass.getResourceAsStream("/opencode-provider-catalog.json")).bufferedReader().use { it.readText() })

    @Test fun providerCatalogFixtureMatchesTheSharedCopy() {
        val shared = generateSequence(java.io.File("").absoluteFile) { it.parentFile }
            .map { java.io.File(it,"tests/fixtures/opencode-provider-catalog.json") }.firstOrNull { it.isFile }
        org.junit.Assume.assumeTrue("The shared fixture is outside this checkout",shared != null)
        assertArrayEquals(shared!!.readBytes(),checkNotNull(javaClass.getResourceAsStream("/opencode-provider-catalog.json")).use { it.readBytes() })
    }

    @Test fun heartbeatAccountParsesProvidersAndState() {
        val catalog = checkNotNull(AccountSnapshots.account(contract.getJSONObject("heartbeat_account"))?.openCode)
        assertEquals(listOf("free","env","config","local","key","oauth"),catalog.providers.map { it.kind })
        assertEquals(2,catalog.providers.single { it.id=="keyed" }.credentials)
        assertEquals("Work",catalog.providers.last().activeLabel)
        assertEquals(1,catalog.providers.single { it.id=="solo" }.models)
        assertEquals("ready",catalog.state); assertFalse(catalog.providersTruncated); assertFalse(catalog.modelsTruncated)
        assertEquals(1792267200.5,catalog.catalogAt!!,0.0)
        assertNull(AccountSnapshots.account(JSONObject().put("id","work").put("provider","codex"))!!.openCode)
    }

    @Test fun launchShortListKeepsOrderMarksAndGroups() {
        val models = OpenCodeCatalog.models(contract.getJSONObject("launch_account").getJSONArray("model_options"))
        assertEquals(listOf("fixture/local","keyed/paid-model","groq/openai/gpt-oss-20b","fixture/alternate","ollama/qwen3-coder:30b",
            "opencode/glm-5.3-flash-free","opencode/big-pickle","groq/groq/compound"),models.map { it.value })
        assertTrue(models[0].isDefault)
        assertEquals(listOf("keyed/paid-model","groq/openai/gpt-oss-20b"),models.filter { it.favorite }.map { it.value })
        val groups = OpenCodeCatalog.group(models)
        assertEquals(listOf("Private keyless fixture","Keyed fixture","Groq","Ollama","OpenCode"),groups.map { it.label })
        assertEquals(listOf("groq/openai/gpt-oss-20b","groq/groq/compound"),groups[2].models.map { it.value })
        assertEquals(listOf("Default"),OpenCodeCatalog.marks(models[0]))
        assertEquals(listOf("Free"),OpenCodeCatalog.marks(models[5]))
        assertEquals(listOf("low","high","max"),models[5].efforts.map { it.value })
    }

    @Test fun searchMatchesEveryWordAcrossLabelValueAndProvider() {
        val models = OpenCodeCatalog.models(contract.getJSONObject("launch_account").getJSONArray("model_options"))
        assertEquals(listOf("groq/openai/gpt-oss-20b"),OpenCodeCatalog.filter(models,"gpt 20B").map { it.value })
        assertEquals(listOf("fixture/alternate"),OpenCodeCatalog.filter(models,"keyless alternate").map { it.value })
        assertEquals(8,OpenCodeCatalog.filter(models,"  ").size)
    }

    @Test fun stoppedAccountExplainsStateAndTruncation() {
        val catalog = checkNotNull(AccountSnapshots.account(contract.getJSONObject("stopped_account"))?.openCode)
        assertEquals("OpenCode is not running on fixture",OpenCodeCatalog.stateMessage(catalog.state,catalog.reason))
        assertEquals("OpenCode is not running on this computer.",OpenCodeCatalog.stateMessage("not_running",""))
        assertEquals("",OpenCodeCatalog.stateMessage("ready",""))
        assertEquals("Showing a shortened list. Choose other models on the desktop. Some providers are not shown.",
            OpenCodeCatalog.truncationMessage(catalog.modelsTruncated,catalog.providersTruncated))
    }

    @Test fun hostileEntriesAreDroppedOrBounded() {
        val providers = JSONArray().put(JSONObject().put("id","bad id").put("kind","key")).put("text")
            .put(JSONObject().put("id","ok").put("label","\u001b").put("kind","magic").put("credential_count",-1).put("model_count",20000))
        val parsed = OpenCodeCatalog.providers(providers)
        assertEquals(1,parsed.size); assertEquals("ok",parsed[0].label); assertEquals("",parsed[0].kind)
        assertNull(parsed[0].credentials); assertNull(parsed[0].models)
        assertEquals("Connected",OpenCodeCatalog.kindLabel(parsed[0].kind))
        val models = JSONArray().put(JSONObject().put("value","noslash")).put(JSONObject().put("value","p/"+"m".repeat(200)))
            .put(JSONObject().put("value","p/ok").put("favorite","yes"))
        assertEquals(listOf("p/ok"),OpenCodeCatalog.models(models).map { it.value })
        assertFalse(OpenCodeCatalog.models(models).single().favorite)
    }

    @Test fun persistedAccountsKeepAtMostSixteenProviders() {
        val raw = JSONObject(contract.getJSONObject("heartbeat_account").toString())
        raw.put("providers",JSONArray((0 until 64).map { JSONObject().put("id","p$it").put("label","P$it").put("kind","env").put("credential_count",0).put("model_count",1) }))
        val account = checkNotNull(AccountSnapshots.account(raw))
        assertEquals(64,account.openCode!!.providers.size)
        val restored = checkNotNull(AccountSnapshots.account(AccountSnapshots.accountJson(account)))
        assertEquals(16,restored.openCode!!.providers.size); assertTrue(restored.openCode!!.providersTruncated)
    }

    @Test fun olderCachesWithoutOpenCodeFieldsStillDecode() {
        val old = JSONObject().put("id","native-opencode").put("provider","opencode").put("label","OpenCode").put("usage",JSONObject().put("status","unavailable"))
        val account = checkNotNull(AccountSnapshots.account(old))
        assertEquals(emptyList<OpenCodeProvider>(),account.openCode!!.providers)
        assertEquals("",account.openCode!!.state)
    }
}
