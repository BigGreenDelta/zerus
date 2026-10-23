package app.zerus.mobile

import org.json.JSONArray
import org.json.JSONObject
import java.time.Instant

/** A display-only projection. Never retain provider payloads, credentials or arbitrary source strings. */
data class AccountIdentity(val name:String="",val email:String="",val organization:String="",val plan:String="",val authMethod:String="",val accountId:String="")
data class AccountWindow(val id:String,val label:String,val usedPercent:Double?,val minutes:Double?,val resetsAt:Double?)
data class AccountBalance(val kind:String,val balance:String?,val currency:String)
data class ReportedAccount(val id:String,val provider:String,val label:String,val native:Boolean,val installed:Boolean,val isDefault:Boolean,
    val status:String,val authStatus:String,val identity:AccountIdentity,val windows:List<AccountWindow>,val creditBalance:String?,val unlimited:Boolean?,
    val balances:List<AccountBalance>,val checkedAt:Double?,val identityCached:Boolean,val refreshing:Boolean,val refreshError:Boolean,val stale:Boolean,val openCode:OpenCodeAccountCatalog?=null)
data class AccountCatalog(val key:MachineKey,val machineName:String,val supported:Boolean,val available:Boolean,val stale:Boolean,val online:Boolean,
    val observedAt:Double?,val accounts:List<ReportedAccount>)

object AccountSnapshots {
    private val accountId=Regex("[A-Za-z0-9_][A-Za-z0-9_-]{0,79}")
    private val providers=setOf("codex","claude","kimi","dsh","opencode")
    private val statuses=setOf("ok","loading","configured","offline","expired","signed_out","credentials_locked","desktop_session_unavailable","credentials_unavailable","unavailable","error","unknown")
    private fun str(raw:JSONObject,key:String,max:Int=256):String = (raw.opt(key) as? String)?.takeIf { it.length<=max && it.none { c -> c.isISOControl() && c!='\n' } }.orEmpty()
    private fun number(raw:JSONObject,key:String):Double?=(raw.opt(key) as? Number)?.toDouble()?.takeIf { it.isFinite() && it>=0 && it<=9_007_199_254_740_991.0 }
    private fun money(raw:JSONObject,key:String):String?=(raw.opt(key) as? String)?.takeIf { it.length<=80 && it.matches(Regex("[+-]?[0-9]+(?:\\.[0-9]+)?")) }?.let { runCatching { java.math.BigDecimal(it).stripTrailingZeros().toPlainString() }.getOrNull() }
    private fun flag(raw:JSONObject,key:String)=raw.opt(key) as? Boolean == true
    private fun reset(raw:JSONObject):Double? = number(raw,"resets_at") ?: (raw.opt("resets_at") as? String)?.takeIf { it.length<=80 }?.let {
        runCatching { Instant.parse(it).epochSecond.toDouble() }.getOrNull()?.takeIf { value -> value>=0 }
    }
    fun parse(connectionId:String,computer:JSONObject):AccountCatalog {
        val key=MachineKey(connectionId,str(computer,"id"));val snapshot=computer.optJSONObject("snapshot")
        val raw=snapshot?.optJSONObject("mobile_accounts")
        val declared=snapshot?.optJSONObject("mobile_capabilities")?.optJSONArray("features")?.let { values -> (0 until values.length()).any { values.opt(it)=="accounts_snapshot" } } == true
        val valid=declared && raw?.opt("schema") is Number && raw.optDouble("schema")==1.0
        val online=flag(computer,"online")
        if(!valid || raw.toString().toByteArray(Charsets.UTF_8).size>128*1024) return AccountCatalog(key,str(computer,"name"),false,false,true,online,null,emptyList())
        val list=raw.optJSONArray("accounts")
        val eligible=flag(raw,"available") && list!=null && list.length()<=100
        val accounts=if(eligible) (0 until list!!.length()).mapNotNull { list.optJSONObject(it)?.let(::account) }.distinctBy { it.provider to it.id } else emptyList()
        val complete=eligible && accounts.size==list!!.length()
        return AccountCatalog(key,str(computer,"name"),true,complete,flag(raw,"stale") || !online || !complete,online,number(raw,"checked_at"),accounts)
    }
    internal fun account(raw:JSONObject):ReportedAccount? {
        val id=str(raw,"id",80);val provider=str(raw,"provider",16)
        if(!accountId.matches(id) || provider !in providers) return null
        val usage=raw.optJSONObject("usage") ?: JSONObject();val identity=usage.optJSONObject("identity") ?: JSONObject()
        val windows=usage.optJSONArray("windows")?.let { rows -> (0 until minOf(rows.length(),16)).mapNotNull { index -> rows.optJSONObject(index)?.let {
            AccountWindow(str(it,"id",80),str(it,"label",160),number(it,"used_percent"),number(it,"window_minutes"),reset(it))
        } } }.orEmpty()
        val balances=usage.optJSONArray("balances")?.let { rows -> (0 until minOf(rows.length(),16)).mapNotNull { index -> rows.optJSONObject(index)?.let {
            AccountBalance(str(it,"kind",80),money(it,"balance"),str(it,"currency",16).takeIf { currency -> currency in setOf("USD","CNY") }.orEmpty())
        } } }.orEmpty()
        val credits=usage.optJSONObject("credits") ?: JSONObject()
        return ReportedAccount(id,provider,str(raw,"label",160).ifBlank { id },flag(raw,"native"),flag(raw,"installed"),flag(raw,"is_default"),
            str(usage,"status",48).takeIf { it in statuses } ?: "unknown",str(usage,"auth_status",48).takeIf { it in setOf("signed_in","signed_out","expired","locked","unavailable","unknown","configured","credentials_locked") } ?: "unknown",
            AccountIdentity(str(identity,"name"),str(identity,"email"),str(identity,"organization"),str(identity,"plan"),str(identity,"auth_method",80),str(identity,"account_id")),
            windows,money(credits,"balance"),credits.opt("unlimited") as? Boolean,balances,number(usage,"checked_at"),flag(usage,"identity_cached"),flag(usage,"refreshing"),flag(usage,"refresh_error"),flag(usage,"stale"),OpenCodeCatalog.account(raw))
    }
    /** A successful catalog is authoritative for removals. Failed workspaces keep explicitly stale values. */
    fun merge(previous:List<AccountCatalog>,incoming:List<AccountCatalog>,successful:Set<String>,connected:Set<String>):List<AccountCatalog> {
        val old=previous.associateBy { it.key }
        val updated=incoming.filter { it.key.connectionId in connected }.map { next ->
            val prior=old[next.key]
            when {
                prior!=null && next.observedAt!=null && prior.observedAt!=null && next.observedAt<prior.observedAt -> prior.copy(stale=true,online=next.online)
                prior!=null && !next.available -> next.copy(accounts=prior.accounts,stale=true)
                else -> next
            }
        }
        return (updated + previous.filter { it.key.connectionId in connected && it.key.connectionId !in successful }.map { it.copy(online=false,stale=true) })
            .distinctBy { it.key }.take(64)
    }
    fun encode(catalogs:List<AccountCatalog>):JSONArray=JSONArray(catalogs.take(64).map { catalog ->
        JSONObject().put("schema",1).put("connection",catalog.key.connectionId).put("computer",catalog.key.computerId).put("name",catalog.machineName)
            .put("supported",catalog.supported).put("available",catalog.available).put("stale",catalog.stale).put("checked_at",catalog.observedAt)
            .put("accounts",JSONArray(catalog.accounts.map(::accountJson)))
    }).also { require(it.toString().toByteArray(Charsets.UTF_8).size<=1024*1024) { "Account cache exceeds its private storage limit" } }
    internal fun accountJson(a:ReportedAccount):JSONObject = JSONObject().put("id",a.id).put("provider",a.provider).put("label",a.label).put("native",a.native).put("installed",a.installed).put("is_default",a.isDefault).put("usage",
        JSONObject().put("status",a.status).put("auth_status",a.authStatus).put("identity",JSONObject().put("name",a.identity.name).put("email",a.identity.email).put("organization",a.identity.organization).put("plan",a.identity.plan).put("auth_method",a.identity.authMethod).put("account_id",a.identity.accountId))
            .put("windows",JSONArray(a.windows.map { JSONObject().put("id",it.id).put("label",it.label).put("used_percent",it.usedPercent).put("window_minutes",it.minutes).put("resets_at",it.resetsAt) }))
            .put("credits",JSONObject().put("balance",a.creditBalance).put("unlimited",a.unlimited)).put("balances",JSONArray(a.balances.map { JSONObject().put("kind",it.kind).put("balance",it.balance).put("currency",it.currency) }))
            .put("checked_at",a.checkedAt).put("identity_cached",a.identityCached).put("refreshing",a.refreshing).put("refresh_error",a.refreshError).put("stale",a.stale))
        .also { row -> a.openCode?.let { catalog ->
            row.put("catalog_state",catalog.state).put("catalog_reason",catalog.reason).put("catalog_at",catalog.catalogAt)
                .put("model_options_truncated",catalog.modelsTruncated)
                .put("providers_truncated",catalog.providersTruncated || catalog.providers.size>OpenCodeCatalog.PERSISTED_PROVIDERS)
                .put("providers",JSONArray(catalog.providers.take(OpenCodeCatalog.PERSISTED_PROVIDERS).map { provider ->
                    JSONObject().put("id",provider.id).put("label",provider.label).put("kind",provider.kind)
                        .put("credential_count",provider.credentials).put("active_label",provider.activeLabel).put("model_count",provider.models)
                }))
        } }
    fun decode(rows:JSONArray):List<AccountCatalog> {
        if(rows.length()>64 || rows.toString().toByteArray(Charsets.UTF_8).size>1024*1024) return emptyList()
        return (0 until rows.length()).mapNotNull { index -> rows.optJSONObject(index)?.let { raw ->
            if(raw.opt("schema") !is Number || raw.optDouble("schema")!=1.0) return@let null
            val connection=str(raw,"connection");val computer=str(raw,"computer");val accounts=raw.optJSONArray("accounts") ?: return@let null
            if(connection.isBlank() || computer.isBlank() || accounts.length()>100) return@let null
            AccountCatalog(MachineKey(connection,computer),str(raw,"name"),flag(raw,"supported"),flag(raw,"available"),true,false,number(raw,"checked_at"),
                (0 until accounts.length()).mapNotNull { accounts.optJSONObject(it)?.let(::account) }.distinctBy { it.provider to it.id })
        } }.distinctBy { it.key }
    }
    fun demo(now:Double):List<AccountCatalog> {
        val payload=JSONObject().put("schema",1).put("available",true).put("checked_at",now).put("accounts",JSONArray()
            .put(JSONObject().put("id","sample").put("provider","codex").put("installed",true).put("native",true).put("label","Personal (preview)").put("is_default",true).put("usage",JSONObject().put("status","ok").put("checked_at",now).put("identity",JSONObject().put("name","Example account").put("plan","Example plan").put("account_id","example-personal").put("email","personal@example.com")).put("windows",JSONArray().put(JSONObject().put("id","current").put("label","Session").put("used_percent",42).put("window_minutes",300).put("resets_at",now+7200)).put(JSONObject().put("id","ended").put("label","Weekly").put("used_percent",91).put("window_minutes",10080).put("resets_at",now-60)))))
            .put(JSONObject().put("id","unknown").put("provider","claude").put("installed",true).put("native",true).put("label","Work (preview)").put("usage",JSONObject().put("status","unavailable").put("identity",JSONObject().put("plan","Not reported")))))
        fun catalog(id:String,name:String,online:Boolean,accounts:JSONArray)=parse("demo",JSONObject().put("id",id).put("name",name).put("online",online).put("snapshot",JSONObject().put("mobile_accounts",JSONObject(payload.toString()).put("accounts",accounts)).put("mobile_capabilities",JSONObject().put("features",JSONArray().put("accounts_snapshot")))))
        fun alias(profile:String,accountId:String)=JSONObject(payload.getJSONArray("accounts").getJSONObject(0).toString()).put("id",profile).put("label","Default account").also {
            it.getJSONObject("usage").getJSONObject("identity").put("account_id",accountId).put("email","shared@example.com")
        }
        val local=payload.getJSONArray("accounts")
        local.put(alias("shared-one","example-shared-one")).put(alias("shared-two","example-shared-two"))
        val laptop=JSONArray().put(JSONObject(local.getJSONObject(0).toString()).put("id","laptop-personal").put("is_default",false))
            .put(alias("shared-email",""))
        return listOf(catalog("demo","Preview machine",true,local),catalog("demo-laptop","Preview laptop",true,laptop),
            catalog("demo-build","Preview build machine with a long display name",false,JSONArray().put(JSONObject(local.getJSONObject(0).toString()).put("id","build-personal").also { it.getJSONObject("usage").put("refresh_error",true) })))
    }
}

data class AccountSelection(val machine:MachineKey,val provider:String,val accountId:String)

object AccountPresentation {
    fun selected(catalogs:List<AccountCatalog>,selection:AccountSelection):Pair<AccountCatalog,ReportedAccount>? {
        val catalog=catalogs.find { it.key==selection.machine } ?: return null
        return catalog.accounts.find { it.provider==selection.provider && it.id==selection.accountId }?.let { catalog to it }
    }
    fun resetSummary(window:AccountWindow,now:Double):String=when(val remaining=remaining(window,now)) {
        "Unknown" -> "Reset unknown"
        "Ended" -> "Window ended"
        else -> "Resets in $remaining"
    }
    fun remaining(window:AccountWindow,now:Double):String {
        val reset=window.resetsAt?.takeIf { it.isFinite() && it>=0 && it<253402300800.0 } ?: return "Unknown"
        if(!now.isFinite()) return "Unknown"
        if(reset<=now) return "Ended"
        val minutes=kotlin.math.ceil((reset-now)/60.0).toLong()
        val remaining=when {
            minutes>=1440 -> "${minutes/1440}d"+(if(minutes%1440/60>0) " ${minutes%1440/60}h" else "")
            minutes>=60 -> "${minutes/60}h"+(if(minutes%60>0) " ${minutes%60}m" else "")
            else -> "${minutes}m"
        }
        return remaining
    }
    private fun authenticationProblem(account:ReportedAccount):String=when(account.authStatus) {
        "signed_in" -> ""
        "expired" -> "Sign-in expired"
        "signed_out" -> "Signed out"
        "locked","credentials_locked" -> "Credentials locked"
        else -> when(account.status) {
            // Older Claude producers called an expired usage access token an
            // expired sign-in. Only explicit native auth can establish that.
            "expired" -> if(account.provider=="claude") "" else "Sign-in expired"
            "signed_out" -> "Signed out"
            "credentials_locked" -> "Credentials locked"
            "desktop_session_unavailable" -> "Desktop session unavailable"
            "credentials_unavailable" -> "Credentials unavailable"
            else -> ""
        }
    }
    fun warning(account:ReportedAccount)=authenticationProblem(account).isNotBlank()
    /** The list reports account access problems; optional usage diagnostics belong in details. */
    fun listProblem(account:ReportedAccount)=authenticationProblem(account)
    fun refreshing(catalog:AccountCatalog,account:ReportedAccount)=catalog.online && !catalog.stale && account.refreshing
    fun type(account:ReportedAccount)=account.identity.plan.ifBlank { account.identity.authMethod }
    fun compactStatus(catalog:AccountCatalog,account:ReportedAccount,now:Double?=null):String {
        val auth=authenticationProblem(account)
        val lastKnown=!catalog.online || catalog.stale || account.stale || account.refreshError ||
            (now!=null && stale(catalog,account,now))
        val problem=if(auth.isNotBlank()) (if(lastKnown) "Last reported: " else "")+auth
            else when {
                account.status in setOf("expired","unavailable") && account.authStatus=="signed_in" ->
                    (if(lastKnown) "Last reported: " else "")+"Signed in / Usage unavailable"
                account.status=="expired" -> "Usage unavailable"
                account.status=="error" || account.status=="offline" -> status(account)
                else -> ""
            }
        val offline=if(!catalog.online) "Offline" else ""
        return listOf(problem,offline,if(!account.installed) "Provider not installed" else "",if(refreshing(catalog,account)) "Updating provider report" else if(account.refreshError) "Provider refresh failed" else "").filter(String::isNotBlank).joinToString(" / ")
    }
    fun freshness(catalog:AccountCatalog,account:ReportedAccount,now:Double):String {
        val values=if(account.windows.isNotEmpty() || account.creditBalance!=null || account.unlimited!=null || account.balances.isNotEmpty()) "Showing saved values" else "No saved usage values"
        val state=when {
            !catalog.online -> "Saved report · Machine offline"
            refreshing(catalog,account) -> "Updating provider report · $values"
            account.refreshError -> "Provider refresh failed · $values"
            account.status in setOf("unavailable","expired","error","signed_out","credentials_locked","desktop_session_unavailable","credentials_unavailable","unknown") -> "Usage unavailable · $values"
            account.status=="loading" -> "Waiting for provider report"
            account.status=="offline" -> "Provider unavailable · $values"
            account.status=="configured" -> "Configured · No usage report"
            stale(catalog,account,now) -> "Saved report"
            else -> "Current report"
        }
        val at=account.checkedAt?.takeIf { it<=now+60 }?.let(::date)
        return state+if(at==null) " · Provider update time unknown" else " · Provider updated $at"
    }
    fun reportedStatus(catalog:AccountCatalog,account:ReportedAccount,now:Double):String =
        (if(stale(catalog,account,now)) "Last reported: " else "")+status(account)
    fun stale(catalog:AccountCatalog,account:ReportedAccount,now:Double):Boolean = !catalog.online || catalog.stale || account.stale || account.refreshError || account.checkedAt==null || account.checkedAt>now+60 || now-account.checkedAt>600
    fun ended(window:AccountWindow,now:Double)=window.resetsAt?.let { it<=now } == true
    fun percent(value:Double?)=value?.let { java.math.BigDecimal.valueOf(it).setScale(1,java.math.RoundingMode.HALF_UP).stripTrailingZeros().toPlainString()+"% used" } ?: "Usage unknown"
    fun date(seconds:Double?):String?=seconds?.takeIf { it.isFinite() && it>=0 && it<253402300800.0 }?.let {
        runCatching { java.time.format.DateTimeFormatter.ofLocalizedDateTime(java.time.format.FormatStyle.MEDIUM,java.time.format.FormatStyle.SHORT).withZone(java.time.ZoneId.systemDefault()).format(Instant.ofEpochSecond(it.toLong())) }.getOrNull()
    }
    private fun duration(window:AccountWindow):String {
        val n=window.minutes?.takeIf { it>0 && it<=52560000 && it%1==0.0 }?.toLong()
        return n?.let { when { it%1440L==0L -> "${it/1440}d";it%60L==0L -> "${it/60}h";else -> "${it}m" } }.orEmpty()
    }
    fun shortPeriod(window:AccountWindow):String=duration(window).ifBlank {
        val label=window.label.ifBlank { "Window" }
        val count=label.codePointCount(0,label.length)
        if(count<=12) label else label.substring(0,label.offsetByCodePoints(0,12))+"…"
    }
    fun period(window:AccountWindow):String {
        return listOf(window.label.ifBlank { "Usage window" },duration(window)).filter(String::isNotBlank).joinToString(" ")
    }
    fun status(account:ReportedAccount):String=authenticationProblem(account).ifBlank {
        when(account.status) {
            "ok" -> "Reported usage";"configured" -> "Configured";"loading" -> "Loading provider data"
            "offline" -> "Provider offline";else -> "Usage unavailable"
        }
    }
}
