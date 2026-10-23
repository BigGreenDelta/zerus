package app.zerus.mobile

import org.json.JSONObject
import java.util.UUID

/** Native checkout reservations are durable actions; recovery only reads the original clone. */
internal object RepositoryAcquisition {
    const val clone = "repository_clone"
    val operations = setOf("repository_check", clone, "repository_status")
    const val uncertain = "Checkout unconfirmed. Check its original result; it has not been retried."
    fun suggestedName(url:String):String = url.trim().trimEnd('/').substringAfterLast('/').substringAfterLast(':').removeSuffix(".git")
        .takeIf { it.isNotBlank() && it !in setOf(".","..") && it.none { c -> c.isISOControl() || c in "/\\:*?\"<>|" } }.orEmpty()
    fun destination(parent:String,name:String) = (if(parent=="/") "/" else parent.trimEnd('/')+"/")+name
    fun validUrl(url:String):Boolean {
        if(url.isEmpty() || url.toByteArray(Charsets.UTF_8).size>4096 || url.any { it.isWhitespace() || it.isISOControl() } ||
            url.contains('\\') || url.startsWith("//") || Regex("^[A-Za-z]:.*").matches(url)) return false
        if(url.startsWith("https://")) {
            val rest=url.removePrefix("https://");val host=rest.substringBefore('/')
            return host.isNotEmpty() && !host.contains('@') && rest.contains('/') && rest.none { it in "?#" }
        }
        if(url.startsWith("ssh://")) {
            val rest=url.removePrefix("ssh://");val host=rest.substringBefore('/')
            return host.isNotEmpty() && host.substringAfterLast('@').isNotEmpty() &&
                (!host.contains('@') || !host.substringBefore('@').contains(':')) && rest.contains('/') && rest.none { it in "?#" }
        }
        if(url.contains("://") || url.startsWith('-') || url.contains("::")) return false
        val parts=url.split(':',limit=2)
        return parts.size==2 && parts[0].isNotEmpty() && !parts[0].contains('/') && parts[1].isNotEmpty()
    }
    fun arguments(machineId:String,url:String,path:String):JSONObject {
        require(runCatching { UUID.fromString(machineId).toString()==machineId }.getOrDefault(false)) { "Refresh this computer's identity." }
        require(validUrl(url)) { "Use an SSH or HTTPS repository URL without embedded credentials." }
        require(DirectoryCreation.pathError(path).isBlank()) { "Enter an absolute Clone to path." }
        val name=path.substringAfterLast('/')
        require(name.toByteArray(Charsets.UTF_8).size<=240 && name==name.trim() && !name.endsWith('.') && name.none { it in "/\\:*?\"<>|" }) { "Choose a valid new folder name." }
        return JSONObject().put("expected_machine_id",machineId).put("url",url).put("parent",path.substringBeforeLast('/').ifEmpty { "/" }).put("name",name)
    }
    fun verifiedCheckout(raw:JSONObject?,path:String) = WorktreePresentation.forFolder(raw,path)?.string("path")==path && WorktreePresentation.contains(raw,path)
    /** Only the read-only check may resolve an alias; its visible destination needs explicit confirmation. */
    data class CheckedDestination(val arguments:JSONObject,val path:String,val needsConfirmation:Boolean)
    fun checkedDestination(args:JSONObject,receipt:JSONObject,id:String):CheckedDestination {
        val raw=receipt.optJSONObject("result")
        require(receipt.string("request_id")==id && receipt.string("state")=="completed" && raw!=null && raw.opt("schema")==1 &&
            raw.string("request_id")==id && raw.string("clone_request_id")==id && raw.string("machine_id")==args.string("expected_machine_id") &&
            raw.string("status")=="ready" && raw.string("name")==args.string("name")) { "Repository or destination could not be verified." }
        val checked=arguments(args.getString("expected_machine_id"),args.getString("url"),raw.getString("path"))
        require(checked.string("parent")==raw.string("parent") && checked.string("name")==args.string("name")) { "Invalid checked destination." }
        val path=destination(checked.getString("parent"),checked.getString("name"))
        return CheckedDestination(checked,path,path!=destination(args.getString("parent"),args.getString("name")))
    }
    data class RecoveryPage(val selected:SessionAction?,val others:List<SessionAction>,val next:String?)
    /** UUID order gives stable bounded pages while new results reorder the durable action list. */
    fun recovery(actions:List<SessionAction>,machine:MachineKey,path:String,before:String="",limit:Int=5):RecoveryPage {
        val target=Target(machine.computerId,"","","",machine.connectionId)
        fun matches(action:SessionAction)=action.operation==clone && action.target==target
        val selected=actions.lastOrNull { action -> matches(action) && (path.isEmpty() ||
            runCatching { JSONObject(action.arguments).let { destination(it.getString("parent"),it.getString("name"))==path } }.getOrDefault(false)) }
        val size=limit.coerceIn(1,10)
        val page=mutableListOf<SessionAction>()
        for(action in actions) if(matches(action) && action.blocksSending && action.requestId!=selected?.requestId && (before.isEmpty() || action.requestId<before)) {
            page.add(action);page.sortByDescending { it.requestId };if(page.size>size+1) page.removeAt(page.lastIndex)
        }
        return RecoveryPage(selected,page.take(size),if(page.size>size) page[size-1].requestId else null)
    }
    fun pending(actions:List<SessionAction>,machine:MachineKey,path:String?=null)=actions.lastOrNull { action ->
        action.operation==clone && action.target==Target(machine.computerId,"","","",machine.connectionId) && action.blocksSending &&
            (path==null || runCatching { JSONObject(action.arguments).let { val reserved=destination(it.getString("parent"),it.getString("name"));DirectoryCreation.pathError(reserved).isNotBlank() || reserved==path } }.getOrDefault(true))
    }
    fun statusPayload(action:SessionAction,id:String):JSONObject {
        require(action.operation==clone && UUID.fromString(id).toString()==id && id!=action.requestId)
        return JSONObject(action.arguments).put("request_id",id).put("clone_request_id",action.requestId)
    }
    fun result(action:SessionAction,receipt:JSONObject,readId:String=action.requestId):SessionAction {
        if(action.target!=Target(action.target.computerId,"","","",action.target.connectionId) ||
            !runCatching { UUID.fromString(action.requestId).toString()==action.requestId }.getOrDefault(false)) return action.copy(status="uncertain",error=uncertain)
        if(readId==action.requestId && receipt.string("request_id")==readId && receipt.string("state")=="failed")
            return action.copy(status="failed",error=receipt.string("error").ifEmpty { "Checkout was refused." })
        val p=runCatching { JSONObject(action.arguments) }.getOrNull()
        val raw=receipt.optJSONObject("result")
        if(p==null || action.target.session.isNotEmpty() || receipt.string("request_id")!=readId || receipt.string("state")!="completed" || raw==null ||
            raw.opt("schema")!=1 || raw.string("request_id")!=readId || raw.string("clone_request_id")!=action.requestId ||
            raw.string("machine_id")!=p.string("expected_machine_id") || raw.string("parent")!=p.string("parent") || raw.string("name")!=p.string("name") ||
            raw.string("path")!=destination(p.string("parent"),p.string("name"))) return action.copy(status="uncertain",error=uncertain)
        return when(raw.string("status")) {
            "cloned" -> action.copy(status="completed",error="",resultPath=raw.string("path"))
            "failed" -> action.copy(status="failed",error=raw.string("detail").ifEmpty { "Checkout failed. Inspect the destination before choosing another folder." })
            else -> action.copy(status="uncertain",error=uncertain)
        }
    }
}
