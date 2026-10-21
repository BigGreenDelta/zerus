package app.zerus.mobile

import org.json.JSONObject
import org.json.JSONArray
import android.icu.lang.UCharacter

data class LaunchAccount(val id: String,val provider: String,val label: String,val native: Boolean=false,val isDefault: Boolean=false,val installed:Boolean=true,val authStatus:String="unknown",val usage:ReportedAccount?=null)
data class LaunchAccountChoice(val account:LaunchAccount,val title:String,val limits:String,val usage:ReportedAccount?=null) {
    val key get() = account.provider+"/"+account.id
}
enum class LaunchLimitTone { Normal, Warning, Critical }
data class LaunchLimitPace(val used:Double,val elapsed:Double?,val tone:LaunchLimitTone)
/** One OpenCode variant of a model, offered as launch effort. */
data class LaunchEffort(val value:String,val label:String)
/** One native effort: a validated ID and the computer's label when it reported a usable one. */
data class NativeEffort(val value:String,val label:String?)
/** The one reader of `effort_options`/`effort_labels` for New session and the model sheet. */
object NativeEfforts {
    const val MAX=16
    /**
     * The native catalog rule every layer shares (tests/fixtures/opencode-effort-ids.json): non-empty, at most
     * 40 UTF-8 bytes, no whitespace, control character or `/`. Membership in the model's own list stays authoritative.
     */
    fun valid(value:String) = value.isNotEmpty() && value.toByteArray(Charsets.UTF_8).size<=40 && value.none { it.isWhitespace() || it.isISOControl() || it=='/' }
    /** Valid IDs in native order, first occurrence, at most [MAX]; labels come from `effort_labels` at the same index. */
    fun parse(option:JSONObject?):List<NativeEffort> {
        val ids=option?.optJSONArray("effort_options") ?: return emptyList()
        val labels=option.optJSONArray("effort_labels")
        return (0 until ids.length()).asSequence().mapNotNull { index ->
            val id=(ids.opt(index) as? String)?.takeIf(::valid) ?: return@mapNotNull null
            NativeEffort(id,(labels?.opt(index) as? String)?.takeIf { it.isNotBlank() && it.length<=80 && it.none(Char::isISOControl) })
        }.distinctBy { it.value }.take(MAX).toList()
    }
}
data class LaunchModel(val value:String,val label:String,val efforts:List<LaunchEffort> = emptyList(),
    val provider:String="",val providerLabel:String="",val free:Boolean=false,val favorite:Boolean=false,val isDefault:Boolean=false)
data class LaunchFolder(val path: String,val name: String)
data class LaunchProjectFolder(val id:String,val path:String,val name:String)
data class LaunchProject(val id:String,val name:String,val color:String,val folders:List<LaunchProjectFolder>)
/** Everything that can keep New session from starting; [LaunchPresentation.disabledReason] explains the first problem. */
data class LaunchReadiness(val machineReason:String="",val catalogLoading:Boolean=false,val catalogAvailable:Boolean=true,val catalogChanged:Boolean=false,
    val projectRequired:Boolean=false,val projectChosen:Boolean=true,val projectHasFolder:Boolean=true,val folderChosen:Boolean=true,
    val folderLoading:Boolean=false,val folderVerified:Boolean=true,val repositoryLoading:Boolean=false,val providers:Int=1,val providerChosen:Boolean=true,
    val accounts:Int=1,val accountChosen:Boolean=true,val nameError:String="",val automatic:Boolean=false,val automaticFailed:Boolean=false,val automaticUsed:Boolean=false,
    val worktreeReason:String="",val confirmed:Boolean=false)
object LaunchPresentation {
    /** Catalog order is kept; the query matches project names and this computer's folder names and paths. */
    fun filterProjects(projects:List<LaunchProject>,query:String):List<LaunchProject> {
        val needle=query.trim()
        if(needle.isEmpty()) return projects
        return projects.filter { project -> project.name.contains(needle,true) ||
            project.folders.any { it.name.contains(needle,true) || it.path.contains(needle,true) } }
    }
    fun folderSummary(project:LaunchProject):String = when(project.folders.size) {
        0 -> "No folder on this computer"
        1 -> project.folders.single().path
        else -> "${project.folders.size} folders"
    }
    /** One concise reason for a disabled Create session action, or "" when it can start. */
    fun disabledReason(form:LaunchReadiness):String = when {
        form.confirmed -> "This session was already created."
        form.machineReason.isNotBlank() -> form.machineReason
        form.catalogLoading -> "Loading agents and projects…"
        !form.catalogAvailable -> "Agents and projects are unavailable. Close and try again."
        form.catalogChanged -> "Creation choices changed. Review them first."
        form.projectRequired && !form.projectChosen -> "Choose a project."
        !form.folderChosen -> if(form.projectHasFolder) "Choose a folder." else "Browse to choose a folder."
        form.folderLoading -> "Checking folder…"
        !form.folderVerified -> "Check this folder again or browse to it."
        form.repositoryLoading -> "Checking repository…"
        form.providers == 0 -> "No agents are installed on this computer."
        !form.providerChosen -> "Choose an agent."
        form.accounts == 0 -> "No account is available for this agent on this computer."
        !form.accountChosen -> "Choose an account."
        form.nameError.isNotBlank() -> form.nameError
        form.automatic && form.automaticFailed -> "Automatic worktree creation failed. Choose None or another worktree."
        form.automatic && form.automaticUsed -> "This form already created its worktree. Choose None to start there."
        form.automatic && form.worktreeReason.isNotBlank() -> form.worktreeReason
        else -> ""
    }
    fun agents(raw: JSONObject?): List<String> = raw?.optJSONArray("agents")?.let { array ->
        (0 until array.length()).mapNotNull { array.opt(it) as? String }.filter { it in setOf("codex","claude","kimi","dsh","opencode") }.distinct()
    }.orEmpty()
    fun accounts(raw: JSONObject?,provider: String): List<LaunchAccount> = raw?.optJSONArray("accounts")?.objects().orEmpty().mapNotNull { row ->
        val id = row.opt("id") as? String
        val agent = row.opt("provider") as? String
        val label = row.opt("label") as? String
        if(id.isNullOrBlank() || !id.matches(Regex("[A-Za-z0-9_][A-Za-z0-9_-]{0,79}")) || agent != provider || label == null) null else LaunchAccount(id,agent,label,
            row.opt("native") == true,row.opt("is_default") == true,row.opt("installed") != false,
            (row.opt("auth_status") as? String)?.takeIf { it in setOf("signed_in","signed_out","expired","credentials_locked","desktop_session_unavailable","credentials_unavailable") } ?: "unknown",
            if(row.optJSONObject("usage") != null) AccountSnapshots.account(row) else null)
    }.distinctBy { it.id }
    /** Relay and connector feature: `launch` and `settings` accept an OpenCode variant of the model as `effort`. */
    const val OPENCODE_LAUNCH_EFFORT = "opencode_launch_effort"
    /** Both the relay and the computer's connector must advertise it; older ones refuse native variant names. */
    fun effortSupported(relayFeatures:Set<String>,computerFeatures:Set<String>) = OPENCODE_LAUNCH_EFFORT in relayFeatures && OPENCODE_LAUNCH_EFFORT in computerFeatures
    fun validModel(value:String) = value.isNotBlank() && value.length<=120 && value.contains('/') && value.none(Char::isISOControl)
    fun validEffort(value:String) = NativeEfforts.valid(value)
    /** The model's own variants in native order; a missing label shows the ID. */
    /** Only exact models offered by this computer's selected native OpenCode account. */
    fun models(raw:JSONObject?,provider:String,accountId:String):List<LaunchModel> {
        if(provider!="opencode") return emptyList()
        val account=raw?.optJSONArray("accounts")?.objects().orEmpty().singleOrNull { it.string("provider")==provider && it.string("id")==accountId } ?: return emptyList()
        return OpenCodeCatalog.models(account.optJSONArray("model_options")).map { model ->
            LaunchModel(model.value,model.label,model.efforts.map { LaunchEffort(it.value,it.label ?: it.value) },
                model.provider,model.providerLabel,model.free,model.favorite,model.isDefault)
        }
    }
    fun catalogNotice(raw:JSONObject?,provider:String,accountId:String):String {
        if(provider!="opencode") return ""
        val row=raw?.optJSONArray("accounts")?.objects().orEmpty().singleOrNull { it.string("provider")==provider && it.string("id")==accountId } ?: return ""
        val catalog=OpenCodeCatalog.account(row) ?: return ""
        return listOf(OpenCodeCatalog.stateMessage(catalog.state,catalog.reason),OpenCodeCatalog.truncationMessage(catalog.modelsTruncated,false))
            .filter(String::isNotBlank).joinToString(" ")
    }

    /**
     * Effort choices for New session: the chosen OpenCode model's variants, only when the relay and the
     * connector advertise [OPENCODE_LAUNCH_EFFORT] ([effortSupported]). Empty means no Effort row and no `effort` in the request.
     */
    fun efforts(raw:JSONObject?,provider:String,accountId:String,model:String,supported:Boolean):List<LaunchEffort> =
        if(!supported || provider!="opencode" || model.isBlank()) emptyList()
        else models(raw,provider,accountId).find { it.value==model }?.efforts.orEmpty()
    /** The checked native launch request for the current form choices. */
    fun arguments(catalog:JSONObject,agent:String,directory:String,tag:String,accountId:String?,projectId:String,worktrees:JSONObject?,
        model:String="",effort:String="",effortSupported:Boolean=false):JSONObject {
        check(catalog.optJSONArray("agents")?.let { array -> (0 until array.length()).any { array.optString(it) == agent } } == true) { "This native provider is unavailable." }
        check(accountId != null && accounts(catalog,agent).any { it.id == accountId }) { "Choose an offered account for the selected provider." }
        val args=JSONObject().put("agent",agent).put("directory",directory).put("tag",tag).put("account_id",accountId)
        if(model.isNotBlank()) {
            check(models(catalog,agent,accountId).any { it.value==model }) { "The native model changed. Review the current choices." }
            args.put("model",model)
        }
        if(effort.isNotBlank()) {
            check(effortSupported) { "This computer does not accept an OpenCode effort at launch. Choose Model default." }
            check(efforts(catalog,agent,accountId,model,true).any { it.value==effort }) { "The model's effort choices changed. Review the current choices." }
            args.put("effort",effort)
        }
        if(catalog.opt("project_launch_supported")==true) projectArguments(catalog,projectId,directory,worktrees).let { fields -> fields.keys().forEach { key -> args.put(key,fields.get(key)) } }
        else check(projectId.isBlank()) { "This computer does not support scoped project assignment." }
        return SessionActionPolicies.arguments("launch",args)
    }
    fun defaultAccount(accounts:List<LaunchAccount>,selected:String=""):String = accounts.firstOrNull { it.id == selected }?.id
        ?: accounts.singleOrNull { it.isDefault }?.id ?: accounts.firstOrNull()?.id.orEmpty()
    fun accountTitle(account:LaunchAccount):String = account.usage?.identity?.email?.takeIf(String::isNotBlank)
        ?: account.label.takeUnless { it.isBlank() || it in setOf("Default account","Native account") } ?: "Account n/a"
    /** Exact machine/profile cache reads only; recomposition never fetches provider limits. */
    fun accountChoices(raw:JSONObject?,machine:MachineKey,catalogs:List<AccountCatalog>,now:Double,
        selectedProvider:String="",selectedAccount:String="",fold:(String)->String={ UCharacter.foldCase(it,true) }):List<LaunchAccountChoice> {
        val cache=catalogs.find { it.key==machine }?.accounts.orEmpty()
        val local=agents(raw).flatMap { provider -> accounts(raw,provider) }.map { account ->
            val cached=cache.find { it.provider==account.provider && it.id==account.id }
                ?.takeIf { account.authStatus in setOf("unknown","signed_in") &&
                    (account.usage==null || account.usage.identity.email.isNotBlank() && account.usage.identity.email==it.identity.email &&
                        account.usage.identity.accountId==it.identity.accountId && account.usage.identity.organization==it.identity.organization) }
            val reported=if(cached!=null && (account.usage==null || (cached.checkedAt ?: 0.0)>(account.usage.checkedAt ?: 0.0))) cached else account.usage
            val snapshot=reported ?: AccountSnapshots.account(JSONObject().put("id",account.id).put("provider",account.provider).put("label",account.label))!!
            account to snapshot.copy(id=account.id,provider=account.provider,label=account.label,native=account.native,isDefault=account.isDefault,installed=account.installed)
        }
        val catalog=AccountCatalog(machine,"",true,true,false,true,null,local.map { it.second })
        // Match Accounts grouping within the selected computer; never borrow
        // another computer's binding, even when its provider identity matches.
        return AccountGrouping.group(listOf(catalog),fold).map { group ->
            val members=group.members.map { member -> local.single { it.first.provider==member.account.provider && it.first.id==member.account.id }.first }
            val chosen=members.firstOrNull { it.provider==selectedProvider && it.id==selectedAccount }
                ?: members.firstOrNull { it.isDefault } ?: members.firstOrNull { it.native } ?: members.first()
            val reported=group.snapshot.account
            LaunchAccountChoice(chosen,accountTitle(chosen.copy(usage=reported)),accountLimits(reported,now),reported)
        }
    }
    fun limitWindows(account:ReportedAccount?,now:Double)=account?.windows.orEmpty().filter {
        it.usedPercent?.isFinite()==true && !AccountPresentation.ended(it,now)
    }
    /** Same quota/elapsed rails and pace colors as the desktop footer. */
    fun limitPace(window:AccountWindow,now:Double):LaunchLimitPace {
        val used=window.usedPercent?.takeIf { it.isFinite() }?.coerceIn(0.0,100.0) ?: 0.0
        val threshold=if(used>=90) LaunchLimitTone.Critical else if(used>=70) LaunchLimitTone.Warning else LaunchLimitTone.Normal
        val length=(window.minutes ?: 0.0)*60
        val left=(window.resetsAt ?: now)-now
        if(!length.isFinite() || length<=0 || !left.isFinite() || left<=0) return LaunchLimitPace(used,null,threshold)
        val passed=(length-left).coerceAtLeast(0.0);val elapsed=passed/length
        if(used>=100 || elapsed<.08) return LaunchLimitPace(used,elapsed,threshold)
        val projected=used/elapsed
        val tone=if(projected>100) {
            val runsOut=(100-used)*passed/used
            if(runsOut>=left/2) LaunchLimitTone.Warning else LaunchLimitTone.Critical
        } else if(used>=90) LaunchLimitTone.Warning else LaunchLimitTone.Normal
        return LaunchLimitPace(used,elapsed,tone)
    }
    fun accountLimits(account:ReportedAccount?,now:Double):String {
        val limits=limitWindows(account,now).map { window ->
            val used=window.usedPercent!!
            "${AccountPresentation.shortPeriod(window)} ${(used.coerceIn(0.0,100.0)+.5).toInt()}% used"
        } + account?.balances.orEmpty().mapNotNull { balance -> balance.balance?.let { "Balance: $it ${balance.currency}".trim() } }
        return limits.joinToString("  ").ifBlank { "limits n/a" }
    }
    fun projects(raw:JSONObject?):List<LaunchProject> {
        if(raw?.opt("project_launch_supported") != true || (raw.opt("swarm_id") as? String).isNullOrBlank()) return emptyList()
        return raw.optJSONArray("projects")?.objects().orEmpty().mapNotNull { row ->
            val id=row.opt("id") as? String;val name=row.opt("name") as? String
            if(id.isNullOrBlank() || id.trim()!=id || id.toByteArray(Charsets.UTF_8).size>128 || id.any(Char::isISOControl) || name.isNullOrBlank() || row.opt("accessible")==false) null else LaunchProject(id,name,row.string("color"),
                row.optJSONArray("folders")?.objects().orEmpty().mapNotNull { folder ->
                    val folderId=folder.opt("id") as? String;val path=folder.opt("path") as? String
                    if(folderId.isNullOrBlank() || folderId.trim()!=folderId || folderId.toByteArray(Charsets.UTF_8).size>128 || path==null || !absoluteFolder(path) || folder.opt("local")==false) null else LaunchProjectFolder(folderId,path,folder.string("name").ifBlank { path.substringAfterLast('/') })
                }.distinctBy { it.id })
        }.distinctBy { it.id }
    }
    fun projectArguments(raw:JSONObject,projectId:String,directory:String,worktrees:JSONObject?=null):JSONObject {
        val project=projects(raw).singleOrNull { it.id == projectId } ?: error("Choose a current native project.")
        val folder=project.folders.singleOrNull { it.path == directory }
        val anchor = if (folder == null && raw.opt("worktree_supported") == true) WorktreePresentation.anchor(worktrees, project, directory) else null
        return JSONObject().put("swarm_id",raw.getString("swarm_id")).put("project_id",project.id).put("add_folder",folder==null && anchor==null)
            .also { if(folder!=null) it.put("project_folder_id",folder.id)
                else if(anchor!=null) it.put("worktree_folder_id",anchor.id).put("worktree_common_dir",worktrees!!.getString("common_dir")) }
    }
    fun sanitizeCatalog(raw:JSONObject):JSONObject {
        val result=JSONObject().put("agents",JSONArray(agents(raw)))
        result.put("accounts",JSONArray(agents(raw).flatMap { provider -> accounts(raw,provider) }.map { account ->
            JSONObject().put("id",account.id).put("provider",account.provider).put("label",account.label)
                .put("native",account.native).put("is_default",account.isDefault).put("installed",account.installed).put("auth_status",account.authStatus)
                .put("model_options",JSONArray(models(raw,account.provider,account.id).map { model -> JSONObject().put("value",model.value).put("label",model.label)
                    .put("provider",model.provider).put("provider_label",model.providerLabel).put("free",model.free)
                    .also { row ->
                        if(model.favorite) row.put("favorite",true)
                        if(model.isDefault) row.put("default",true)
                        if(model.efforts.isNotEmpty()) row.put("effort_options",JSONArray(model.efforts.map { it.value })).put("effort_labels",JSONArray(model.efforts.map { it.label }))
                    } }))
                .also { row -> if(account.provider=="opencode") {
                    raw.optJSONArray("accounts")?.objects().orEmpty().singleOrNull { it.string("provider")==account.provider && it.string("id")==account.id }
                        ?.let(OpenCodeCatalog::account)?.let { catalog -> row.put("catalog_state",catalog.state).put("catalog_reason",catalog.reason).put("model_options_truncated",catalog.modelsTruncated) }
                } }
                .also { row -> account.usage?.let { row.put("usage",AccountSnapshots.accountJson(it).getJSONObject("usage")) } }
        }))
        val supported=raw.opt("project_launch_supported")==true
        result.put("project_launch_supported",supported).put("worktree_supported",raw.opt("worktree_supported") == true)
        if(supported) {
            result.put("swarm_id",raw.string("swarm_id")).put("default_project",raw.string("default_project"))
            result.put("projects",JSONArray(projects(raw).map { project -> JSONObject().put("id",project.id).put("name",project.name).put("color",project.color)
                .put("folders",JSONArray(project.folders.map { folder -> JSONObject().put("id",folder.id).put("name",folder.name).put("path",folder.path) })) }))
        }
        return result
    }
    fun folders(raw: JSONObject?): List<LaunchFolder> = raw?.optJSONArray("directories")?.objects().orEmpty().mapNotNull { row ->
        val path = row.opt("path") as? String
        if(path == null || !absoluteFolder(path)) null else LaunchFolder(path,(row.opt("name") as? String).orEmpty().ifBlank { path.substringAfterLast('/') })
    }.distinctBy { it.path }
    fun absoluteFolder(path: String) = path.startsWith('/') && path.length <= 4096 && path.none(Char::isISOControl)
    fun selectedFolder(raw: JSONObject?,selected: String): Boolean = absoluteFolder(selected) &&
        (raw?.opt("path") == selected || raw?.opt("requested_path") == selected || folders(raw).any { it.path == selected })
    fun generatedName() = "work-" + java.util.UUID.randomUUID().toString().replace("-", "").take(8)
    fun nameError(tag: String): String = when {
        tag.isBlank() -> "Enter a session name."
        tag != tag.trim() -> "Remove spaces at the beginning and end."
        tag.toByteArray(Charsets.UTF_8).size > 120 -> "Session name exceeds 120 UTF-8 bytes."
        tag.any { it == '/' || it == '\\' || it == ':' || it == '.' || it.isISOControl() } -> "Use a name without path separators, dots, colons, or control characters."
        else -> ""
    }
}
