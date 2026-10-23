package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class AccountSnapshotsTest {
    private fun account(id:String="profile",provider:String="codex",usage:JSONObject=JSONObject())=JSONObject().put("id",id).put("provider",provider).put("label","Same label").put("usage",usage)
    private fun computer(accounts:JSONArray=JSONArray().put(account()),schema:Any=1,id:String="node",checked:Double=1000.0)=JSONObject().put("id",id).put("name","Reported machine").put("online",true).put("snapshot",JSONObject().put("mobile_capabilities",JSONObject().put("features",JSONArray().put("accounts_snapshot"))).put("mobile_accounts",JSONObject().put("schema",schema).put("available",true).put("checked_at",checked).put("accounts",accounts)))
    private fun parse(usage:JSONObject)=AccountSnapshots.parse("workspace",computer(JSONArray().put(account(usage=usage))))
    @Test fun accountListShowsAccessProblemsRatherThanOptionalUsageDiagnostics() {
        val a=parse(JSONObject().put("status","unavailable").put("auth_status","signed_in").put("refresh_error",true)).accounts.single()
        assertEquals("",AccountPresentation.listProblem(a))
        assertEquals("",AccountPresentation.listProblem(a.copy(status="error",refreshing=true)))
        assertFalse(AccountPresentation.warning(a.copy(status="error")))
        assertEquals("Signed out",AccountPresentation.listProblem(a.copy(authStatus="signed_out")))
        assertEquals("Credentials locked",AccountPresentation.listProblem(a.copy(authStatus="locked")))
    }
    @Test fun quotasRemainUnknownWithoutStrictFiniteNumbers() {
        val rows=parse(JSONObject().put("windows",JSONArray().put(JSONObject().put("used_percent",true).put("resets_at","bad")).put(JSONObject().put("used_percent","42")).put(JSONObject().put("used_percent",-1)).put(JSONObject().put("used_percent",150)))).accounts.single().windows
        assertNull(rows[0].usedPercent);assertNull(rows[0].resetsAt);assertNull(rows[1].usedPercent);assertNull(rows[2].usedPercent)
        assertEquals("150% used",AccountPresentation.percent(rows[3].usedPercent));assertEquals("Usage unknown",AccountPresentation.percent(null))
        assertNull(parse(JSONObject().put("checked_at","NaN")).accounts.single().checkedAt);assertNull(parse(JSONObject().put("checked_at",true)).accounts.single().checkedAt)
        assertNull(AccountPresentation.date(9_007_199_254_740_991.0))
    }
    @Test fun catalogTimeDoesNotFabricateUsageFreshnessAndEndedWindowKeepsValue() {
        val c=parse(JSONObject().put("status","ok").put("windows",JSONArray().put(JSONObject().put("label","Weekly").put("used_percent",91).put("window_minutes",10080).put("resets_at","1970-01-01T00:10:00Z"))))
        val a=c.accounts.single();assertNull(a.checkedAt);assertTrue(AccountPresentation.stale(c,a,1000.0));assertTrue(AccountPresentation.ended(a.windows.single(),1000.0))
        assertEquals("91% used",AccountPresentation.percent(a.windows.single().usedPercent));assertEquals("Weekly 7d",AccountPresentation.period(a.windows.single()))
        val future=parse(JSONObject().put("checked_at",1061));assertTrue(AccountPresentation.stale(future,future.accounts.single(),1000.0))
        val fresh=parse(JSONObject().put("checked_at",999));assertFalse(AccountPresentation.stale(fresh,fresh.accounts.single(),1000.0))
    }
    @Test fun decimalMoneyAndSignedInSurviveAllowlistedCache() {
        val usage=JSONObject().put("status","ok").put("auth_status","signed_in").put("checked_at",990).put("identity",JSONObject().put("name","Synthetic person").put("plan","Pro").put("api_key","NEVER_RETAIN")).put("credits",JSONObject().put("balance","-12.3400")).put("balances",JSONArray().put(JSONObject().put("kind","wallet").put("balance","1000000000000000000.123456789").put("currency","USD"))).put("source","https://private.example/path")
        val encoded=AccountSnapshots.encode(listOf(parse(usage)));assertFalse(encoded.toString().contains("NEVER_RETAIN"));assertFalse(encoded.toString().contains("private.example"))
        val c=AccountSnapshots.decode(encoded).single();val a=c.accounts.single();assertEquals("-12.34",a.creditBalance);assertEquals("1000000000000000000.123456789",a.balances.single().balance);assertEquals("signed_in",a.authStatus);assertEquals("Pro",a.identity.plan);assertTrue(c.stale);assertFalse(c.online)
    }
    @Test fun malformedDuplicateAndFutureSchemasNeverClaimVerifiedEmpty() {
        assertFalse(AccountSnapshots.parse("w",computer(schema=2)).supported);assertFalse(AccountSnapshots.parse("w",computer(schema=true)).supported)
        assertFalse(AccountSnapshots.parse("w",computer(JSONArray().put(JSONObject().put("provider","codex")))).available)
        assertFalse(AccountSnapshots.parse("w",computer(JSONArray().put(account()).put(account()))).available)
        val c=AccountSnapshots.parse("w",computer(JSONArray()));assertTrue(c.available);assertTrue(c.accounts.isEmpty())
        assertTrue(AccountSnapshots.decode(JSONArray().put(AccountSnapshots.encode(listOf(c)).getJSONObject(0).put("schema",2))).isEmpty())
        assertFalse(AccountSnapshots.parse("w",JSONObject().put("id","node")).supported)
    }
    @Test fun workspaceMachineProviderAndAccountIdsStayDistinct() {
        val a=AccountSnapshots.parse("a",computer(JSONArray().put(account("one")).put(account("two")).put(account("one","claude"))))
        val rows=AccountSnapshots.merge(emptyList(),listOf(a,AccountSnapshots.parse("b",computer()),AccountSnapshots.parse("a",computer(id="other"))),setOf("a","b"),setOf("a","b"))
        assertEquals(3,rows.size);assertEquals(3,rows.first().accounts.size);assertEquals(3,AccountSnapshots.decode(AccountSnapshots.encode(rows)).size)
    }
    @Test fun removalsStayRemovedAfterRestartAndOlderReadsCannotReplaceNewer() {
        val old=AccountSnapshots.parse("w",computer(checked=1000.0));val fresh=AccountSnapshots.parse("w",computer(JSONArray(),checked=2000.0))
        val removed=AccountSnapshots.merge(listOf(old),listOf(fresh),setOf("w"),setOf("w"));assertTrue(AccountSnapshots.decode(AccountSnapshots.encode(removed)).single().accounts.isEmpty())
        assertTrue(AccountSnapshots.merge(removed,listOf(old),setOf("w"),setOf("w")).single().accounts.isEmpty())
        assertTrue(AccountSnapshots.merge(listOf(old),emptyList(),emptySet(),emptySet()).isEmpty());assertTrue(AccountSnapshots.merge(listOf(old),emptyList(),setOf("w"),setOf("w")).isEmpty())
        val offline=AccountSnapshots.merge(listOf(old),emptyList(),emptySet(),setOf("w")).single();assertFalse(offline.online);assertTrue(offline.stale);assertEquals(1,offline.accounts.size)
    }
    @Test fun percentageIsCompactAndMalformedCurrencyNeverBecomesALink() {
        assertEquals("42.9% used",AccountPresentation.percent(42.8571428571429))
        val a=parse(JSONObject().put("balances",JSONArray().put(JSONObject().put("balance","1.25").put("currency","https://x")))).accounts.single()
        assertEquals("",a.balances.single().currency)
    }
    @Test fun invalidMoneyIsUnknownAndNoWindowsDoesNotImplyZeroQuota() {
        val a=parse(JSONObject().put("credits",JSONObject().put("balance","NaN")).put("balances",JSONArray().put(JSONObject().put("balance",10)))).accounts.single();assertNull(a.creditBalance);assertNull(a.balances.single().balance)
        val unknown=parse(JSONObject()).accounts.single();assertTrue(unknown.windows.isEmpty());assertEquals("Usage unavailable",AccountPresentation.status(unknown))
    }
    @Test fun remainingTimeRoundsUpAndPreservesEndedUnknownBoundaries() {
        fun window(at:Double?)=AccountWindow("window","Session",42.0,300.0,at)
        assertEquals("1m",AccountPresentation.remaining(window(1000.001),1000.0))
        assertEquals("1m",AccountPresentation.remaining(window(1060.0),1000.0))
        assertEquals("1h 1m",AccountPresentation.remaining(window(4600.001),1000.0))
        assertEquals("2h 10m",AccountPresentation.remaining(window(8800.0),1000.0))
        assertEquals("2d 3h",AccountPresentation.remaining(window(184600.0),1000.0))
        assertEquals("Ended",AccountPresentation.remaining(window(1000.0),1000.0))
        assertEquals("Ended",AccountPresentation.remaining(window(999.0),1000.0))
        assertEquals("Unknown",AccountPresentation.remaining(window(null),1000.0))
        assertEquals("Unknown",AccountPresentation.remaining(window(Double.NaN),1000.0))
        assertEquals("Unknown",AccountPresentation.remaining(window(9_007_199_254_740_991.0),1000.0))
    }
    @Test fun detailSelectionResolvesExactLatestTupleAndDisappearsOnRemoval() {
        val a=AccountSnapshots.parse("w",computer(JSONArray().put(account("one")).put(account("two")).put(account("one","claude"))))
        val b=AccountSnapshots.parse("other",computer())
        val otherNode=AccountSnapshots.parse("w",computer(id="other-node"))
        val selection=AccountSelection(a.key,"codex","one")
        val latest=a.copy(accounts=a.accounts.map { if(it.id=="one" && it.provider=="codex") it.copy(label="Updated label") else it })
        assertEquals("Updated label",AccountPresentation.selected(listOf(b,otherNode,latest),selection)!!.second.label)
        assertNull(AccountPresentation.selected(listOf(b,otherNode),selection))
        assertNull(AccountPresentation.selected(listOf(a.copy(accounts=a.accounts.filterNot { it.id=="one" && it.provider=="codex" })),selection))
    }
    @Test fun compactSummaryRetainsProblemsAndPlanOrAuthenticationFallback() {
        val c=parse(JSONObject().put("status","ok").put("auth_status","expired").put("checked_at",1000).put("identity",JSONObject().put("auth_method","API key")))
        val a=c.accounts.single().copy(installed=true)
        assertEquals("API key",AccountPresentation.type(a));assertEquals("Pro",AccountPresentation.type(a.copy(identity=a.identity.copy(plan="Pro"))))
        assertTrue(AccountPresentation.compactStatus(c.copy(online=false),a).contains("Sign-in expired"))
        assertTrue(AccountPresentation.compactStatus(c.copy(online=false),a).contains("Offline"))
        assertEquals("Provider offline",AccountPresentation.compactStatus(c,a.copy(authStatus="signed_in",status="offline")))
    }

    @Test fun nativeSignedInWinsOverLegacyExpiredUsageTokenAndUnavailableQuota() {
        val c=AccountSnapshots.parse("w",computer(JSONArray().put(account(provider="claude",usage=JSONObject()
            .put("status","expired").put("auth_status","signed_in").put("checked_at",1000)))))
        val a=c.accounts.single().copy(installed=true)
        assertFalse(AccountPresentation.warning(a))
        assertEquals("Usage unavailable",AccountPresentation.status(a))
        assertEquals("Signed in / Usage unavailable",AccountPresentation.compactStatus(c,a,1001.0))
        assertFalse(AccountPresentation.compactStatus(c,a.copy(refreshError=true),1001.0).contains("Sign-in expired"))
        assertTrue(AccountPresentation.compactStatus(c,a.copy(refreshError=true),1001.0).contains("Provider refresh failed"))
        assertEquals("Usage unavailable",AccountPresentation.status(a.copy(authStatus="unknown")))
        assertEquals("Signed in / Usage unavailable",AccountPresentation.compactStatus(c,a.copy(status="unavailable"),1001.0))
    }
    @Test fun confirmedAuthProblemsRemainManualAndStaleReportsAreExplicit() {
        val c=parse(JSONObject().put("status","signed_out").put("auth_status","signed_out").put("checked_at",1000))
        val a=c.accounts.single().copy(installed=true)
        assertTrue(AccountPresentation.warning(a))
        assertEquals("Signed out",AccountPresentation.compactStatus(c,a,1001.0))
        assertEquals("Last reported: Signed out / Offline",AccountPresentation.compactStatus(c.copy(online=false),a,1001.0))
        assertEquals("Last reported: Signed out",AccountPresentation.compactStatus(c,a,1700.0))
        assertEquals("Sign-in expired",AccountPresentation.status(a.copy(status="ok",authStatus="expired")))
        assertEquals("Credentials locked",AccountPresentation.status(a.copy(authStatus="locked")))
    }
    @Test fun freshnessSeparatesProviderProgressFailureSavedAndUnknownTimes() {
        val c=parse(JSONObject().put("status","ok").put("checked_at",1000))
        val a=c.accounts.single()
        assertTrue(AccountPresentation.freshness(c,a,1001.0).startsWith("Current report"))
        assertTrue(AccountPresentation.freshness(c,a.copy(refreshing=true),1001.0).startsWith("Updating provider report"))
        assertTrue(AccountPresentation.freshness(c,a.copy(refreshError=true),1001.0).startsWith("Provider refresh failed"))
        assertTrue(AccountPresentation.freshness(c.copy(online=false),a,1001.0).contains("Machine offline"))
        assertFalse(AccountPresentation.refreshing(c.copy(online=false),a.copy(refreshing=true)))
        assertFalse(AccountPresentation.refreshing(c.copy(stale=true),a.copy(refreshing=true)))
        assertFalse(AccountPresentation.compactStatus(c.copy(online=false),a.copy(refreshing=true),1001.0).contains("Updating"))
        assertTrue(AccountPresentation.freshness(c,a,1700.0).startsWith("Saved report"))
        assertTrue(AccountPresentation.freshness(c,a.copy(checkedAt=null),1001.0).endsWith("Provider update time unknown"))
        assertTrue(AccountPresentation.freshness(c,a.copy(checkedAt=2000.0),1001.0).endsWith("Provider update time unknown"))
        assertTrue(AccountPresentation.freshness(c,a.copy(status="unavailable"),1001.0).startsWith("Usage unavailable"))
    }

    @Test fun shortPeriodUsesOnlyValidWholeMinuteUnitsAndBoundsUnicodeFallback() {
        fun window(minutes:Double?,label:String="Session")=AccountWindow("window",label,0.0,minutes,null)
        assertEquals("5h",AccountPresentation.shortPeriod(window(300.0)))
        assertEquals("7d",AccountPresentation.shortPeriod(window(10080.0)))
        assertEquals("Session",AccountPresentation.shortPeriod(window(300.5)))
        assertEquals("Session",AccountPresentation.shortPeriod(window(Double.NaN)))
        val short=AccountPresentation.shortPeriod(window(null,"😀".repeat(20)))
        assertEquals(13,short.codePointCount(0,short.length))
        assertEquals("😀".repeat(12)+"…",short)
        assertEquals("Session 5h",AccountPresentation.period(window(300.0)))
        assertEquals("Window ended",AccountPresentation.resetSummary(window(300.0).copy(resetsAt=1000.0),1000.0))
    }

}
