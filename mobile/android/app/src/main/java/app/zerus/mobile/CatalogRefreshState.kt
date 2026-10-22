package app.zerus.mobile

/** Phone reads and provider reports have different clocks. Never call a received
 * heartbeat a new provider measurement, or finish a newer read from an old job. */
enum class CatalogRefreshStage(val label:String) {
    Waiting("Waiting"), Capabilities("Checking workspace"), Reports("Reading machine reports"),
    Applying("Applying reports"), Current("Reports received"), Failed("Could not read reports"),
    Interrupted("Update interrupted")
}
data class WorkspaceRefresh(val id:String,val name:String,val stage:CatalogRefreshStage,
    val lastReceivedAt:Double?=null)
data class CatalogRefreshState(val generation:Long=0,val running:Boolean=false,
    val workspaces:List<WorkspaceRefresh> = emptyList()) {
    fun begin(id:Long,workspaces:List<Pair<String,String>>):CatalogRefreshState =
        CatalogRefreshState(id,true,workspaces.map { (key,name) ->
            WorkspaceRefresh(key,name,CatalogRefreshStage.Waiting,this.workspaces.find { it.id==key }?.lastReceivedAt)
        })
    fun stage(id:Long,key:String,stage:CatalogRefreshStage):CatalogRefreshState =
        if(!running || generation!=id) this else copy(workspaces=workspaces.map {
            if(it.id==key) it.copy(stage=stage) else it
        })
    fun finish(id:Long,received:Set<String>,now:Double):CatalogRefreshState =
        if(!running || generation!=id) this else copy(running=false,workspaces=workspaces.map {
            when {
                it.id in received -> it.copy(stage=CatalogRefreshStage.Current,lastReceivedAt=now)
                it.stage==CatalogRefreshStage.Failed -> it
                else -> it.copy(stage=CatalogRefreshStage.Interrupted)
            }
        })
    val failures get()=workspaces.count { it.stage in setOf(CatalogRefreshStage.Failed,CatalogRefreshStage.Interrupted) }
    val summary:String get() {
        if(running) {
            val active=workspaces.firstOrNull { it.stage in setOf(CatalogRefreshStage.Capabilities,CatalogRefreshStage.Reports) }
                ?: workspaces.firstOrNull { it.stage==CatalogRefreshStage.Applying }
            return if(active==null) "Syncing workspace reports" else "${active.name} · ${active.stage.label}"
        }
        return when {
            workspaces.isEmpty() -> ""
            failures>0 -> "$failures workspace update${if(failures==1) "" else "s"} failed · Saved values kept"
            else -> "Workspace reports received"
        }
    }
}
