package app.zerus.mobile

import org.json.JSONObject
import org.json.JSONArray

data class LaunchAccount(val id: String,val provider: String,val label: String,val native: Boolean=false,val isDefault: Boolean=false,val installed:Boolean=true,val authStatus:String="unknown",val usage:ReportedAccount?=null)
data class LaunchAccountChoice(val account:LaunchAccount,val title:String,val limits:String) {
    val key get() = account.provider+"/"+account.id
}
data class LaunchModel(val value:String,val label:String)
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
    fun validModel(value:String) = value.isNotBlank() && value.length<=120 && value.contains('/') && value.none(Char::isISOControl)
    /** Only exact models offered by this computer's selected native OpenCode account. */
    fun models(raw:JSONObject?,provider:String,accountId:String):List<LaunchModel> {
        if(provider!="opencode") return emptyList()
        val account=raw?.optJSONArray("accounts")?.objects().orEmpty().singleOrNull { it.string("provider")==provider && it.string("id")==accountId } ?: return emptyList()
        return account.optJSONArray("model_options")?.objects().orEmpty().take(256).mapNotNull { option ->
            val value=((option.opt("value") ?: option.opt("id")) as? String)?.takeIf(::validModel) ?: return@mapNotNull null
            val label=(option.opt("label") as? String)?.takeIf { it.isNotBlank() && it.length<=160 && it.none(Char::isISOControl) } ?: value
            LaunchModel(value,label)
        }.distinctBy { it.value }
    }
    fun defaultAccount(accounts:List<LaunchAccount>,selected:String=""):String = accounts.firstOrNull { it.id == selected }?.id
        ?: accounts.singleOrNull { it.isDefault }?.id ?: accounts.firstOrNull()?.id.orEmpty()
    fun accountTitle(account:LaunchAccount):String = account.usage?.identity?.email?.takeIf(String::isNotBlank)
        ?: account.label.takeUnless { it.isBlank() || it in setOf("Default account","Native account") } ?: "Account n/a"
    /** Exact machine/profile cache reads only; recomposition never fetches provider limits. */
    fun accountChoices(raw:JSONObject?,machine:MachineKey,catalogs:List<AccountCatalog>,now:Double):List<LaunchAccountChoice> {
        val cache=catalogs.find { it.key==machine }?.accounts.orEmpty()
        return agents(raw).flatMap { provider -> accounts(raw,provider) }.map { account ->
            val cached=cache.find { it.provider==account.provider && it.id==account.id }
                ?.takeIf { account.authStatus in setOf("unknown","signed_in") &&
                    (account.usage==null || account.usage.identity.email.isNotBlank() && account.usage.identity.email==it.identity.email &&
                        account.usage.identity.accountId==it.identity.accountId && account.usage.identity.organization==it.identity.organization) }
            val reported=if(cached!=null && (account.usage==null || (cached.checkedAt ?: 0.0)>(account.usage.checkedAt ?: 0.0))) cached else account.usage
            val title=accountTitle(account.copy(usage=reported))
            LaunchAccountChoice(account,title,accountLimits(reported,now))
        }
    }
    fun accountLimits(account:ReportedAccount?,now:Double):String {
        val limits=account?.windows.orEmpty().mapNotNull { window ->
            val used=window.usedPercent?.takeIf { it.isFinite() } ?: return@mapNotNull null
            if(AccountPresentation.ended(window,now)) return@mapNotNull null
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
                .put("model_options",JSONArray(models(raw,account.provider,account.id).map { JSONObject().put("value",it.value).put("label",it.label) }))
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
