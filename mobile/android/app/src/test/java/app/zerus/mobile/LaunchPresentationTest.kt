package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class LaunchPresentationTest {
    @Test fun openCodeMarksSurviveTheSanitizedCatalog() {
        val contract = JSONObject(checkNotNull(javaClass.getResourceAsStream("/opencode-provider-catalog.json")).bufferedReader().use { it.readText() })
        val raw = JSONObject().put("agents", JSONArray(listOf("opencode"))).put("accounts", JSONArray().put(contract.getJSONObject("stopped_account")))
        val clean = LaunchPresentation.sanitizeCatalog(raw)
        val models = LaunchPresentation.models(clean, "opencode", "native-opencode")
        assertEquals("fixture/local", models.single().value); assertTrue(models.single().isDefault)
        assertEquals("Private keyless fixture", models.single().providerLabel)
        assertEquals("OpenCode is not running on fixture Showing a shortened list. Choose other models on the desktop.",
            LaunchPresentation.catalogNotice(clean, "opencode", "native-opencode"))
        assertEquals("", LaunchPresentation.catalogNotice(clean, "codex", "native-opencode"))
        assertEquals("", LaunchPresentation.catalogNotice(clean, "opencode", "missing"))
    }
    private fun choices(raw:JSONObject?,machine:MachineKey,catalogs:List<AccountCatalog>,now:Double,provider:String="",account:String="")=
        LaunchPresentation.accountChoices(raw,machine,catalogs,now,provider,account) { it.lowercase(java.util.Locale.ROOT) }
    private fun choiceCatalog(usage:JSONObject?=null)=JSONObject().put("agents",JSONArray().put("codex")).put("accounts",JSONArray()
        .put(JSONObject().put("id","one").put("provider","codex").put("label","Alias").put("is_default",true).also { if(usage!=null) it.put("usage",usage) })
        .put(JSONObject().put("id","two").put("provider","codex").put("label","Alias")))
    private fun usage(percent:Double=17.0,reset:Double=2000.0)=JSONObject().put("status","ok").put("checked_at",1000.0)
        .put("identity",JSONObject().put("email","shared@example.invalid"))
        .put("windows",JSONArray().put(JSONObject().put("window_minutes",300).put("used_percent",percent).put("resets_at",reset)))
    private fun cached(machine:MachineKey,usage:JSONObject):AccountCatalog {
        val row=JSONObject().put("id","one").put("provider","codex").put("label","Alias").put("usage",usage)
        return AccountCatalog(machine,"Example",true,true,false,true,1000.0,listOf(AccountSnapshots.account(row)!!))
    }
    @Test fun cachedRowsUseEmailAndOneLimitLineWithoutDefaultOrFailureDetails() {
        val machine=MachineKey("workspace","computer")
        val raw=choiceCatalog(usage().put("refresh_error",true).put("error","private detail"))
        val choices=choices(LaunchPresentation.sanitizeCatalog(raw),machine,emptyList(),1000.0)
        assertEquals("shared@example.invalid",choices[0].title)
        assertEquals("5h 17% used",choices[0].limits)
        assertEquals("limits n/a",choices[1].limits)
        assertEquals(listOf("codex/one","codex/two"),choices.map { it.key })
        assertFalse(choices.any { it.title.contains("default") || it.limits.contains("detail") })
        assertFalse(LaunchPresentation.sanitizeCatalog(raw).toString().contains("private detail"))
    }
    @Test fun privateUsageCacheCannotCrossComputerOrProfileAndAuthChangesRejectOldQuotas() {
        val machine=MachineKey("workspace","computer")
        val cache=cached(machine,usage())
        val choices=choices(choiceCatalog(),machine,listOf(cache),1000.0)
        assertEquals("5h 17% used",choices[0].limits)
        assertEquals("limits n/a",choices[1].limits)
        assertEquals("limits n/a",choices(choiceCatalog(),machine,listOf(cache.copy(key=MachineKey("other","computer"))),1000.0)[0].limits)
        val signedOut=choiceCatalog().also { it.getJSONArray("accounts").getJSONObject(0).put("auth_status","signed_out") }
        assertEquals("limits n/a",choices(signedOut,machine,listOf(cache),1000.0)[0].limits)
        val replaced=choiceCatalog(usage().put("identity",JSONObject().put("email","new@example.invalid")))
        assertEquals("new@example.invalid",choices(replaced,machine,listOf(cache),1000.0)[0].title)
        val newScope=choiceCatalog(usage().put("identity",JSONObject().put("email","shared@example.invalid").put("account_id","new-scope")))
        val oldScope=cached(machine,usage(99.0).put("checked_at",1100.0))
        assertEquals("5h 17% used",choices(newScope,machine,listOf(oldScope),1100.0)[0].limits)
    }
    @Test fun endedAndUnknownCachedWindowsStayUnavailableAndNewerSnapshotsWin() {
        val machine=MachineKey("workspace","computer")
        assertEquals("limits n/a",choices(choiceCatalog(usage(reset=999.0)),machine,emptyList(),1000.0)[0].limits)
        val unknown=usage().also { it.getJSONArray("windows").getJSONObject(0).remove("used_percent") }
        assertEquals("limits n/a",choices(choiceCatalog(unknown),machine,emptyList(),1000.0)[0].limits)
        val newer=cached(machine,usage(29.0).put("checked_at",1100.0))
        assertEquals("5h 29% used",choices(choiceCatalog(usage()),machine,listOf(newer),1100.0)[0].limits)
    }
    @Test fun aliasesMatchAccountsGroupingOnlyOnSelectedComputerAndKeepSelectedBinding() {
        val machine=MachineKey("workspace","computer")
        val raw=choiceCatalog(usage())
        raw.getJSONArray("accounts").getJSONObject(1).put("usage",usage(29.0).put("checked_at",1100.0))
        val merged=choices(raw,machine,emptyList(),1100.0)
        assertEquals(1,merged.size);assertEquals("one",merged.single().account.id);assertEquals("5h 29% used",merged.single().limits)
        assertEquals("two",choices(raw,machine,emptyList(),1100.0,"codex","two").single().account.id)
        val accounts=LaunchPresentation.accounts(raw,"codex").map { it.usage!! }
        val catalog=AccountCatalog(machine,"",true,true,false,true,null,accounts)
        assertEquals(AccountGrouping.group(listOf(catalog)) { it.lowercase(java.util.Locale.ROOT) }.size,merged.size)
        val scoped=JSONObject(raw.toString()).also { it.getJSONArray("accounts").getJSONObject(1).getJSONObject("usage").getJSONObject("identity").put("organization","Other") }
        assertEquals(2,choices(scoped,machine,emptyList(),1100.0).size)
        val ids=JSONObject(raw.toString()).also { it.getJSONArray("accounts").getJSONObject(0).getJSONObject("usage").getJSONObject("identity").put("account_id","first")
            it.getJSONArray("accounts").getJSONObject(1).getJSONObject("usage").getJSONObject("identity").put("account_id","second") }
        assertEquals(2,choices(ids,machine,emptyList(),1100.0).size)
        val one=choiceCatalog().also { it.getJSONArray("accounts").remove(1) }
        val other=cached(MachineKey("workspace","other-computer"),usage(91.0))
        assertEquals(1,choices(one,machine,listOf(other),1100.0).size)
        assertEquals("limits n/a",choices(one,machine,listOf(other),1100.0).single().limits)
        assertTrue(choices(null,machine,listOf(catalog,other),1100.0).isEmpty())
    }
    @Test fun providersAndAccountsComeOnlyFromTypedNativeCatalog() {
        val raw = JSONObject().put("agents",JSONArray().put("codex").put("unknown").put(1).put("codex").put("kimi"))
            .put("accounts",JSONArray().put(JSONObject().put("id","safe-id").put("provider","codex").put("label","Work"))
                .put(JSONObject().put("id",1).put("provider","codex").put("label","Wrong type"))
                .put(JSONObject().put("id","other").put("provider","kimi").put("label","Other")))
        assertEquals(listOf("codex","kimi"),LaunchPresentation.agents(raw))
        assertEquals(listOf(LaunchAccount("safe-id","codex","Work")),LaunchPresentation.accounts(raw,"codex"))
    }
    @Test fun limitBarsUseDesktopFooterPaceAndSkipUnavailableWindows() {
        val now=1000.0
        fun meter(minutes:Double,used:Double,left:Double)=LaunchPresentation.limitPace(AccountWindow("","",used,minutes,now+left),now)
        assertEquals(LaunchLimitTone.Normal,meter(300.0,80.0,30*60.0).tone)
        assertEquals(LaunchLimitTone.Critical,meter(10080.0,48.0,145*3600.0).tone)
        assertEquals(LaunchLimitTone.Warning,meter(10080.0,92.0,3*3600.0).tone)
        assertEquals(LaunchLimitTone.Warning,meter(10080.0,61.0,76*3600.0).tone)
        assertEquals(LaunchLimitTone.Normal,meter(10080.0,6.0,165*3600.0).tone)
        assertEquals(LaunchLimitTone.Critical,meter(300.0,100.0,47*60.0).tone)
        assertEquals(.9,meter(300.0,80.0,30*60.0).elapsed!!,0.0001)
        assertNull(LaunchPresentation.limitPace(AccountWindow("","",78.0,300.0,null),now).elapsed)
        val account=AccountSnapshots.account(JSONObject().put("id","one").put("provider","codex").put("usage",usage()))!!
        val unavailable=account.copy(windows=listOf(AccountWindow("","",null,300.0,now+60),AccountWindow("","",100.0,300.0,now-1)))
        assertTrue(LaunchPresentation.limitWindows(unavailable,now).isEmpty())
        assertEquals("limits n/a",LaunchPresentation.accountLimits(unavailable,now))
    }
    @Test fun selectionMustStillBeInCurrentVerifiedBrowser() {
        val browser = JSONObject().put("path","/srv/projects").put("directories",JSONArray().put(JSONObject().put("path","/srv/projects/example").put("name","Example")))
        assertTrue(LaunchPresentation.selectedFolder(browser,"/srv/projects"))
        assertTrue(LaunchPresentation.selectedFolder(browser,"/srv/projects/example"))
        assertFalse(LaunchPresentation.selectedFolder(browser,"/srv/other"))
        assertFalse(LaunchPresentation.selectedFolder(browser,"~"))
        assertFalse(LaunchPresentation.absoluteFolder("/srv/new\nfolder"))
    }
    @Test fun nativeTagLimitsAreExplicitAndUtf8Bounded() {
        assertEquals("",LaunchPresentation.nameError("mobile launch"))
        assertNotEquals("",LaunchPresentation.nameError("../escape"))
        assertNotEquals("",LaunchPresentation.nameError(" example "))
        assertEquals("",LaunchPresentation.nameError("界".repeat(40)))
        assertNotEquals("",LaunchPresentation.nameError("界".repeat(41)))
    }
}
