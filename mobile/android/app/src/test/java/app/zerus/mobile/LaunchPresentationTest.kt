package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class LaunchPresentationTest {
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
        val choices=LaunchPresentation.accountChoices(LaunchPresentation.sanitizeCatalog(raw),machine,emptyList(),1000.0)
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
        val choices=LaunchPresentation.accountChoices(choiceCatalog(),machine,listOf(cache),1000.0)
        assertEquals("5h 17% used",choices[0].limits)
        assertEquals("limits n/a",choices[1].limits)
        assertEquals("limits n/a",LaunchPresentation.accountChoices(choiceCatalog(),machine,listOf(cache.copy(key=MachineKey("other","computer"))),1000.0)[0].limits)
        val signedOut=choiceCatalog().also { it.getJSONArray("accounts").getJSONObject(0).put("auth_status","signed_out") }
        assertEquals("limits n/a",LaunchPresentation.accountChoices(signedOut,machine,listOf(cache),1000.0)[0].limits)
        val replaced=choiceCatalog(usage().put("identity",JSONObject().put("email","new@example.invalid")))
        assertEquals("new@example.invalid",LaunchPresentation.accountChoices(replaced,machine,listOf(cache),1000.0)[0].title)
        val newScope=choiceCatalog(usage().put("identity",JSONObject().put("email","shared@example.invalid").put("account_id","new-scope")))
        val oldScope=cached(machine,usage(99.0).put("checked_at",1100.0))
        assertEquals("5h 17% used",LaunchPresentation.accountChoices(newScope,machine,listOf(oldScope),1100.0)[0].limits)
    }
    @Test fun endedAndUnknownCachedWindowsStayUnavailableAndNewerSnapshotsWin() {
        val machine=MachineKey("workspace","computer")
        assertEquals("limits n/a",LaunchPresentation.accountChoices(choiceCatalog(usage(reset=999.0)),machine,emptyList(),1000.0)[0].limits)
        val unknown=usage().also { it.getJSONArray("windows").getJSONObject(0).remove("used_percent") }
        assertEquals("limits n/a",LaunchPresentation.accountChoices(choiceCatalog(unknown),machine,emptyList(),1000.0)[0].limits)
        val newer=cached(machine,usage(29.0).put("checked_at",1100.0))
        assertEquals("5h 29% used",LaunchPresentation.accountChoices(choiceCatalog(usage()),machine,listOf(newer),1100.0)[0].limits)
    }
    @Test fun providersAndAccountsComeOnlyFromTypedNativeCatalog() {
        val raw = JSONObject().put("agents",JSONArray().put("codex").put("unknown").put(1).put("codex").put("kimi"))
            .put("accounts",JSONArray().put(JSONObject().put("id","safe-id").put("provider","codex").put("label","Work"))
                .put(JSONObject().put("id",1).put("provider","codex").put("label","Wrong type"))
                .put(JSONObject().put("id","other").put("provider","kimi").put("label","Other")))
        assertEquals(listOf("codex","kimi"),LaunchPresentation.agents(raw))
        assertEquals(listOf(LaunchAccount("safe-id","codex","Work")),LaunchPresentation.accounts(raw,"codex"))
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
