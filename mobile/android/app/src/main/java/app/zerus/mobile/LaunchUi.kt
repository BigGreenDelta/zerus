package app.zerus.mobile

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.ArrowDropDown
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Folder
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.core.graphics.toColorInt
import org.json.JSONObject
import kotlinx.coroutines.delay

private val Amber = Color(0xFFFFCB7D)

@Composable internal fun NewSessionButton(model: ZerusViewModel) {
    var choosing by remember { mutableStateOf(false) }
    IconButton(onClick = { choosing = true },enabled = !model.demo && model.connections.isNotEmpty()) { Icon(Icons.Default.Add,"New session") }
    if(choosing) {
        ObscureConversation()
        ZerusAlertDialog(onDismissRequest = { choosing = false },title = { Text("New session on which machine?") },text = {
            Column(Modifier.heightIn(max = 440.dp).verticalScroll(rememberScrollState()),verticalArrangement = Arrangement.spacedBy(8.dp)) {
                model.machines.forEach { machine ->
                    val key = MachineKey(machine.connectionId,machine.id)
                    val reason = model.launchReason(key)
                    TextButton(onClick = { choosing = false;model.prepareLaunch(key) },enabled = reason.isBlank(),modifier = Modifier.fillMaxWidth()) {
                        Column(Modifier.fillMaxWidth()) {
                            MachineLabel(machine.name,colorHex = model.machineColor(key))
                            if(model.connections.size > 1) Text(model.connections.find { it.id == machine.connectionId }?.displayName.orEmpty(),style = MaterialTheme.typography.labelSmall)
                            if(reason.isNotBlank()) Text(reason,style = MaterialTheme.typography.bodySmall)
                        }
                    }
                }
                if(model.machines.isEmpty()) Text("Refresh your connected machines first.")
            }
        },confirmButton = { TextButton(onClick = { choosing = false }) { Text("Cancel") } })
    }
}

/**
 * Full-screen New session form. Only the close action or Back can leave it; edits ask before they are
 * discarded, and nothing can close it while a worktree or session is being created.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable internal fun LaunchSessionDialog(model: ZerusViewModel) {
    if(!model.launchVisible) return
    val machine = model.launchMachine ?: return
    val form = model.launchFormId
    ObscureConversation()
    var provider by rememberSaveable(form) { mutableStateOf("") }
    var account by rememberSaveable(form) { mutableStateOf("") }
    var selectedModel by rememberSaveable(form) { mutableStateOf("") }
    var selectedEffort by rememberSaveable(form) { mutableStateOf("") }
    var choosingModel by remember(form) { mutableStateOf(false) }
    var projectId by rememberSaveable(form) { mutableStateOf("") }
    var folder by rememberSaveable(form) { mutableStateOf("") }
    var browsedFolder by rememberSaveable(form) { mutableStateOf("") }
    var tag by rememberSaveable(form) { mutableStateOf(model.launchGeneratedName) }
    var autoPreferred by rememberSaveable(form) { mutableStateOf(true) }
    var edited by rememberSaveable(form) { mutableStateOf(false) }
    var appliedWorktree by rememberSaveable(form) { mutableStateOf("") }
    var choosingProject by rememberSaveable(form) { mutableStateOf(false) }
    var choosingWorktree by rememberSaveable(form) { mutableStateOf(false) }
    var creatingWorktree by rememberSaveable(form) { mutableStateOf(false) }
    var browsing by rememberSaveable(form) { mutableStateOf(false) }
    var discarding by rememberSaveable(form) { mutableStateOf(false) }
    var worktreeMenu by remember(form) { mutableStateOf(false) }
    var allowedCatalog by remember(form) { mutableStateOf("") }
    val signature = model.launchCatalogSignature()
    LaunchedEffect(signature) { if(allowedCatalog.isBlank() && signature.isNotBlank()) allowedCatalog = signature }
    val catalog = model.launchCatalogState
    val providers = remember(catalog) { LaunchPresentation.agents(catalog) }
    val accounts = remember(catalog,provider) { LaunchPresentation.accounts(catalog,provider) }
    val modelOptions = remember(catalog,provider,account) { LaunchPresentation.models(catalog,provider,account) }
    LaunchedEffect(provider,account,modelOptions) { if(selectedModel.isNotBlank() && modelOptions.none { it.value==selectedModel }) selectedModel="" }
    val effortSupported = model.launchEffortSupported(machine)
    val effortOptions = remember(catalog,provider,account,selectedModel,effortSupported) { LaunchPresentation.efforts(catalog,provider,account,selectedModel,effortSupported) }
    // Changing the model drops an effort it does not offer; Create never sends a stale one.
    LaunchedEffect(effortOptions) { if(selectedEffort.isNotBlank() && effortOptions.none { it.value==selectedEffort }) selectedEffort="" }
    val effort = selectedEffort.takeIf { chosen -> effortOptions.any { it.value==chosen } }.orEmpty()
    val accountChoices = LaunchPresentation.accountChoices(catalog,machine,model.accountCatalogs,System.currentTimeMillis()/1000.0,provider,account)
    val projects = remember(catalog) { LaunchPresentation.projects(catalog) }
    val projectSupported = catalog?.opt("project_launch_supported") == true
    val project = projects.find { it.id == projectId }
    LaunchedEffect(providers) { if(provider !in providers) provider = providers.firstOrNull().orEmpty() }
    LaunchedEffect(accounts) { account = LaunchPresentation.defaultAccount(accounts,account) }
    LaunchedEffect(projects) {
        if(projects.none { it.id == projectId }) {
            val preferred = model.selectedProject?.key?.takeIf { it.connectionId == machine.connectionId && it.swarmId == catalog?.string("swarm_id") }?.id
                ?: catalog?.string("default_project")
            projectId = projects.find { it.id == preferred }?.id ?: projects.firstOrNull()?.id.orEmpty()
            folder = browsedFolder
        }
    }
    LaunchedEffect(projectId,model.launchDirectoryLoading) {
        if(folder.isBlank() && !model.launchDirectoryLoading) project?.folders?.firstOrNull()?.let {
            folder = it.path;model.browseLaunchDirectory(machine,it.path)
        }
    }
    // A confirmed checkout from Auto or Create manually is selected directly, like desktop selectPath(path, false).
    LaunchedEffect(model.launchCreatedWorktree) {
        val created = model.launchCreatedWorktree
        if(created.isNotBlank() && created != appliedWorktree) {
            appliedWorktree = created;folder = created;browsedFolder = "";autoPreferred = false;creatingWorktree = false;edited = true
        }
    }
    val computer = model.machines.find { it.id == machine.computerId && it.connectionId == machine.connectionId }?.name ?: machine.computerId
    val busy = model.launchBusy
    val supported = model.worktreeOperationsSupported(machine)
    val evidence = WorktreePresentation.forFolder(model.launchWorktreeState,folder)
    val createdEvidence = model.launchCreatedEvidence?.takeIf { folder.isNotBlank() && folder == model.launchCreatedWorktree }
    val placement = WorktreePresentation.placement(evidence,createdEvidence,project,folder)
    val inProject = project?.folders?.any { it.path == folder } == true
    val related = project != null && !inProject && WorktreePresentation.anchor(placement,project,folder) != null
    val autoPossible = supported && WorktreePresentation.automaticAvailable(evidence,folder)
    val automatic = autoPreferred && autoPossible
    val branch = model.launchGeneratedName
    val plannedPath = if(automatic) WorktreePresentation.automaticPath(evidence,branch) else ""
    val plannedAdded = automatic && project != null && evidence != null &&
        WorktreePresentation.anchor(WorktreePresentation.withCreated(evidence,plannedPath),project,plannedPath) == null
    val folderValid = LaunchPresentation.selectedFolder(model.launchDirectoryState,folder)
    val changed = allowedCatalog.isNotBlank() && signature != allowedCatalog
    val machineActions = model.sessionActions.filter { it.target.connectionId == machine.connectionId && it.target.computerId == machine.computerId }
    fun inForm(action: SessionAction) = model.launchBaseline[action.requestId] != action.status
    // This form's own requests show progress on the Create button instead of a receipt notice.
    val pendingWorktree = machineActions.lastOrNull { it.operation == "worktree_create" && it.blocksSending }?.takeIf { !model.worktreeSubmitting }
    val failedWorktree = machineActions.lastOrNull { it.operation == "worktree_create" && it.status == "failed" && inForm(it) }
    val createdWorktree = machineActions.lastOrNull { it.operation == "worktree_create" && it.status == "completed" && it.resultPath.isNotBlank() && inForm(it) }
    val pendingLaunch = machineActions.lastOrNull { it.operation == "launch" && it.blocksSending }?.takeIf { !model.launchSubmitting }
    val confirmed = machineActions.lastOrNull { it.operation == "launch" && it.status == "completed" && it.resultTarget != null && inForm(it) }
    val nameError = LaunchPresentation.nameError(tag)
    val reason = LaunchPresentation.disabledReason(LaunchReadiness(machineReason = model.launchReason(machine),
        catalogLoading = model.launchCatalogLoading,catalogAvailable = catalog != null && allowedCatalog.isNotBlank(),catalogChanged = changed,
        projectRequired = projectSupported,projectChosen = project != null,projectHasFolder = project?.folders?.isNotEmpty() ?: true,
        folderChosen = folder.isNotBlank(),folderLoading = model.launchDirectoryLoading,folderVerified = folderValid,repositoryLoading = model.launchWorktreeLoading,
        providers = providers.size,providerChosen = provider in providers,accounts = accounts.size,accountChosen = accounts.any { it.id == account },
        nameError = nameError,automatic = automatic,automaticFailed = model.launchAutomaticFailed,automaticUsed = model.launchCreatedWorktree.isNotBlank(),worktreeReason = model.worktreeReason(machine),confirmed = confirmed != null))
    val progress = when {
        model.launchStage == "checking" -> "Checking folder…"
        model.launchStage == "launching" || model.launchSubmitting -> "Creating session…"
        model.launchStage == "creating" || model.worktreeSubmitting -> "Creating worktree…"
        else -> ""
    }
    val dirty = confirmed == null && (edited || tag != model.launchGeneratedName)
    fun requestClose() { if(busy) return;if(dirty) discarding = true else model.closeLaunch() }
    fun create() {
        if(busy || reason.isNotBlank()) return
        if(automatic) model.launchWithWorktree(machine,provider,folder,tag,account,allowedCatalog,projectId,selectedModel,effort)
        else model.launchSession(machine,provider,folder,tag,account,allowedCatalog,projectId,placement,selectedModel,effort)
    }
    fun selectFolder(path: String) { folder = path;browsedFolder = "";edited = true;model.browseLaunchDirectory(machine,path) }
    val canChoose = !busy && !model.launchWorktreeLoading && WorktreePresentation.entries(evidence).isNotEmpty()
    val canCreate = !busy && !model.launchWorktreeLoading && autoPossible && model.worktreeReason(machine).isBlank()
    val focus = LocalFocusManager.current
    Dialog(onDismissRequest = ::requestClose,properties = DialogProperties(usePlatformDefaultWidth = false,dismissOnClickOutside = false,decorFitsSystemWindows = false)) {
        Surface(Modifier.fillMaxSize(),color = MaterialTheme.colorScheme.background) {
            Scaffold(containerColor = MaterialTheme.colorScheme.background,topBar = {
                TopAppBar(title = {
                    Row(verticalAlignment = Alignment.CenterVertically,horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        Text("New session",maxLines = 1)
                        MachineLabel(computer,Modifier.weight(1f,fill = false),colorHex = model.machineColor(machine))
                    }
                },navigationIcon = { IconButton(onClick = ::requestClose,enabled = !busy) { Icon(Icons.Default.Close,"Close") } },
                    colors = TopAppBarDefaults.topAppBarColors(containerColor = MaterialTheme.colorScheme.background))
            },bottomBar = {
                LaunchActionBar(progress,if(busy) "" else model.launchError,if(busy) "" else reason,busy,::create)
            }) { padding ->
                Column(Modifier.padding(padding).consumeWindowInsets(padding).fillMaxSize().verticalScroll(rememberScrollState())
                    .padding(horizontal = 16.dp,vertical = 8.dp),verticalArrangement = Arrangement.spacedBy(22.dp)) {
                    if(model.launchCatalogLoading) LinearProgressIndicator(Modifier.fillMaxWidth())
                    pendingWorktree?.let { action -> LaunchNotice(Amber) {
                        Text("Worktree creation is unconfirmed. Check the original result before creating another.",style = MaterialTheme.typography.bodyMedium)
                        PlannedFolder(action)
                        TextButton(onClick = { model.checkSessionAction(action) },enabled = !busy && "action:${action.requestId}" !in model.receiptFlights) { Text("Check original worktree result") }
                    } }
                    failedWorktree?.takeIf { pendingWorktree == null }?.let { action -> LaunchNotice(Amber) {
                        Text("Worktree was not created. ${action.error}".trim(),style = MaterialTheme.typography.bodyMedium)
                        PlannedFolder(action)
                        Text("Nothing was retried. Inspect this folder before using it.",style = MaterialTheme.typography.bodySmall,color = MaterialTheme.colorScheme.onSurfaceVariant)
                        val planned = runCatching { JSONObject(action.arguments).string("destination") }.getOrDefault("")
                        if(LaunchPresentation.absoluteFolder(planned) && planned != folder)
                            TextButton(onClick = { autoPreferred = false;selectFolder(planned) },enabled = !busy && !model.launchDirectoryLoading) { Text("Check this folder") }
                    } }
                    createdWorktree?.let { action -> LaunchNotice(null) {
                        Text("Worktree created on $computer. It stays there if you close this form.",style = MaterialTheme.typography.bodyMedium)
                        SelectionContainer { Text(action.resultPath,style = MaterialTheme.typography.bodySmall,color = MaterialTheme.colorScheme.onSurfaceVariant) }
                        if(action.resultPath != folder) TextButton(onClick = { autoPreferred = false;selectFolder(action.resultPath) },enabled = !busy && !model.launchDirectoryLoading) { Text("Use created worktree") }
                    } }
                    pendingLaunch?.let { action -> LaunchNotice(Amber) {
                        Text("Launch result unknown. A native session may already exist. This request has not been retried.",style = MaterialTheme.typography.bodyMedium)
                        TextButton(onClick = { model.checkSessionAction(action) },enabled = !busy && "action:${action.requestId}" !in model.receiptFlights) { Text("Check original launch result") }
                    } }
                    confirmed?.let { action -> LaunchNotice(MaterialTheme.colorScheme.primary) {
                        Text("$computer confirmed creation of ${action.resultTarget?.session}.",style = MaterialTheme.typography.bodyMedium)
                        if(action.error.isNotBlank() && action.error != model.launchError) Text(action.error,style = MaterialTheme.typography.bodySmall,color = Amber)
                        TextButton(onClick = { model.openSessionActionResult(action) },enabled = !busy) { Text("Open confirmed session") }
                        if(model.actionResultNotice.isNotBlank()) Text(model.actionResultNotice,style = MaterialTheme.typography.bodySmall)
                    } }
                    if(changed) LaunchNotice(Amber) {
                        Text("Creation choices changed. Review them before creating a session.",style = MaterialTheme.typography.bodyMedium)
                        TextButton(onClick = { allowedCatalog = signature;provider = "";account = "";projectId = "";folder = "" },enabled = !busy) { Text("Review updated choices") }
                    }
                    LaunchSection("Project") {
                        LaunchField("Project",project?.name ?: when {
                            model.launchCatalogLoading -> "Loading projects…"
                            projects.isEmpty() -> "No projects available"
                            else -> "Choose project"
                        },!busy && projectSupported && projects.isNotEmpty(),{ choosingProject = true },
                            supporting = project?.let { LaunchPresentation.folderSummary(it) },
                            leading = { Icon(DesktopIcons.Projects,null,tint = projectColor(project?.color,MaterialTheme.colorScheme.onSurfaceVariant)) })
                        if(!projectSupported && !model.launchCatalogLoading && catalog != null)
                            LaunchHint("Project assignment is unavailable on this computer. Update its connector and native CLI to choose a project.")
                    }
                    LaunchSection("Folder") {
                        val projectFolders = project?.folders.orEmpty()
                        if(project != null && projectFolders.isEmpty() && folder.isBlank())
                            LaunchNotice(Amber) { Text("${project.name} has no folder on $computer. Browse to choose one; it will be added to this project.",style = MaterialTheme.typography.bodyMedium) }
                        if(projectFolders.isEmpty() && folder.isBlank()) {
                            Button(onClick = { browsing = true },enabled = !busy,modifier = Modifier.fillMaxWidth().heightIn(min = 52.dp)) {
                                Icon(Icons.Default.Folder,null,Modifier.size(20.dp));Spacer(Modifier.width(8.dp));Text("Browse folders")
                            }
                        } else Row(verticalAlignment = Alignment.CenterVertically,horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            val title = projectFolders.find { it.path == folder }?.name ?: folder.trimEnd('/').substringAfterLast('/').ifBlank { folder.ifBlank { "Choose folder" } }
                            LaunchMenuField("Folder",title,!busy,projectFolders.map { it.path to it.name },folder,{ selectFolder(it) },Modifier.weight(1f),
                                supporting = when { folder.isBlank() -> null;inProject -> "Project folder";related -> "Worktree of this project";else -> "Other folder" },
                                leading = { Icon(DesktopIcons.Folder,null,tint = MaterialTheme.colorScheme.onSurfaceVariant) },
                                choiceSupporting = { it },onEmpty = { browsing = true })
                            OutlinedButton(onClick = { browsing = true },enabled = !busy,modifier = Modifier.heightIn(min = 56.dp),shape = RoundedCornerShape(12.dp)) { Text("Browse") }
                        }
                        if(folder.isNotBlank()) SelectionContainer { Text(folder,style = MaterialTheme.typography.bodySmall,color = MaterialTheme.colorScheme.onSurfaceVariant) }
                        if(model.launchDirectoryLoading && model.launchStage.isEmpty()) LinearProgressIndicator(Modifier.fillMaxWidth())
                        else if(folder.isNotBlank() && !folderValid && !busy) Row(verticalAlignment = Alignment.CenterVertically) {
                            LaunchHint("This folder has not been checked on $computer.",Modifier.weight(1f))
                            TextButton(onClick = { model.browseLaunchDirectory(machine,folder) }) { Text("Check again") }
                        }
                        if(!automatic && project != null && folder.isNotBlank()) {
                            if(related) LaunchHint("Verified worktree of ${project.name}. No project folder will be added.")
                            else if(!inProject) LaunchHint("This folder is outside ${project.name}. Starting here adds it to the project.",color = Amber)
                        }
                    }
                    LaunchSection("Worktree") {
                        Row(verticalAlignment = Alignment.CenterVertically,horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                            val colors = SegmentedButtonDefaults.colors(activeContainerColor = MaterialTheme.colorScheme.primary.copy(alpha = .14f),
                                activeContentColor = MaterialTheme.colorScheme.primary,activeBorderColor = MaterialTheme.colorScheme.primary,
                                inactiveContainerColor = MaterialTheme.colorScheme.surface,inactiveContentColor = MaterialTheme.colorScheme.onSurface,
                                inactiveBorderColor = MaterialTheme.colorScheme.outline)
                            SingleChoiceSegmentedButtonRow(Modifier.weight(1f)) {
                                SegmentedButton(selected = automatic,onClick = { autoPreferred = true;edited = true },shape = SegmentedButtonDefaults.itemShape(0,2),
                                    enabled = !busy && autoPossible,colors = colors,icon = { Icon(DesktopIcons.GitBranch,null,Modifier.size(18.dp)) }) { Text("Auto") }
                                SegmentedButton(selected = !automatic,onClick = { autoPreferred = false;edited = true },shape = SegmentedButtonDefaults.itemShape(1,2),
                                    enabled = !busy,colors = colors,icon = { Icon(DesktopIcons.Folder,null,Modifier.size(18.dp)) }) { Text("None") }
                            }
                            if(supported) Box {
                                IconButton(onClick = { worktreeMenu = true },enabled = canChoose || canCreate) { Icon(Icons.Default.MoreVert,"Worktree options") }
                                DropdownMenu(worktreeMenu,onDismissRequest = { worktreeMenu = false }) {
                                    DropdownMenuItem(text = { Text("Choose existing…") },leadingIcon = { Icon(DesktopIcons.GitBranch,null) },enabled = canChoose,
                                        onClick = { worktreeMenu = false;choosingWorktree = true })
                                    DropdownMenuItem(text = { Text("Create manually…") },leadingIcon = { Icon(Icons.Default.Add,null) },enabled = canCreate,
                                        onClick = { worktreeMenu = false;creatingWorktree = true })
                                }
                            }
                        }
                        if(automatic) Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                            Text("New worktree",style = MaterialTheme.typography.labelMedium,color = MaterialTheme.colorScheme.onSurfaceVariant)
                            SelectionContainer { Text(plannedPath,style = MaterialTheme.typography.bodySmall) }
                            Text("New branch $branch from HEAD, created when the session starts.",style = MaterialTheme.typography.bodySmall,color = MaterialTheme.colorScheme.onSurfaceVariant)
                            if(plannedAdded) LaunchHint("The new worktree will be added to ${project?.name}.",color = Amber)
                        } else if(autoPossible) LaunchHint("The session starts directly in the selected folder.")
                        else WorktreePresentation.automaticNote(model.launchWorktreeState,folder,supported,model.launchWorktreeLoading)
                            .takeIf { it.isNotBlank() }?.let { LaunchHint(it) }
                    }
                    LaunchSection("Account and agent") {
                        LaunchAccountList(accountChoices,provider,account,!busy) { chosen ->
                            provider = chosen.provider;account = chosen.id;edited = true
                        }
                        if(providers.isEmpty() && !model.launchCatalogLoading && catalog != null) LaunchHint("No installed native providers were reported.")
                    }
                    if(provider=="opencode") LaunchSection("Model") {
                        val chosen=modelOptions.find { it.value==selectedModel }
                        OutlinedButton(onClick={ choosingModel=true },enabled=!busy,modifier=Modifier.fillMaxWidth()) {
                            Text(chosen?.label ?: "Native default model",Modifier.weight(1f))
                            Icon(Icons.Default.ArrowDropDown,null)
                        }
                        LaunchPresentation.catalogNotice(catalog,provider,account).takeIf(String::isNotBlank)?.let { LaunchHint(it) }
                        if(effortOptions.isNotEmpty()) LaunchMenuField("Effort",effortOptions.find { it.value==effort }?.label ?: "Model default",!busy,
                            listOf("" to "Model default") + effortOptions.map { it.value to it.label },effort,{ selectedEffort=it;edited=true })
                        LaunchHint("Models configured on $computer. Native default uses OpenCode's configuration.")
                    }
                    LaunchSection("Session name") {
                        OutlinedTextField(tag,{ tag = it },Modifier.fillMaxWidth(),singleLine = true,readOnly = busy,shape = RoundedCornerShape(12.dp),
                            isError = tag.isNotEmpty() && nameError.isNotBlank(),
                            supportingText = if(tag.isNotEmpty() && nameError.isNotBlank()) { { Text(nameError) } } else null,
                            keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),keyboardActions = KeyboardActions(onDone = { focus.clearFocus() }))
                    }
                }
            }
        }
    }
    if(choosingModel) ModalBottomSheet(onDismissRequest={ choosingModel=false }) {
        val options=modelOptions.map { OpenCodeModel(it.value,it.label,it.provider,it.providerLabel,it.free,it.favorite,it.isDefault,
            it.efforts.map { effort -> NativeEffort(effort.value,effort.label) }) }
        OpenCodeModelList(options,selectedModel,{ selectedModel=it;edited=true;choosingModel=false },
            Modifier.fillMaxWidth().heightIn(max=560.dp).padding(16.dp),nativeDefault="Native default model")
    }
    if(choosingProject) ProjectPickerDialog(projects,projectId,{ chosen ->
        choosingProject = false
        if(chosen != projectId) { projectId = chosen;folder = browsedFolder;edited = true }
    }) { choosingProject = false }
    if(choosingWorktree) WorktreeChooserDialog(WorktreePresentation.entries(evidence),folder,{ path ->
        choosingWorktree = false;autoPreferred = false;selectFolder(path)
    }) { choosingWorktree = false }
    if(creatingWorktree && evidence != null) CreateWorktreeDialog(model,machine,evidence,
        tag.trim().replace(Regex("\\s+"),"-").ifBlank { branch }) { if(!model.worktreeSubmitting) creatingWorktree = false }
    if(browsing) LaunchFolderDialog(model,machine,{ selected -> folder = selected;browsedFolder = selected;browsing = false;edited = true }) {
        browsing = false
        // Navigation replaced the verified folder; check the kept selection again so it stays usable.
        if(folder.isNotBlank() && !LaunchPresentation.selectedFolder(model.launchDirectoryState,folder)) model.browseLaunchDirectory(machine,folder)
    }
    if(discarding) ZerusAlertDialog(onDismissRequest = { discarding = false },title = { Text("Discard new session?") },text = {
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Your choices for this session will be lost.")
            createdWorktree?.let { Text("The created worktree and its branch stay on $computer: ${it.resultPath}",style = MaterialTheme.typography.bodySmall) }
        }
    },confirmButton = { TextButton(onClick = { discarding = false;model.closeLaunch() }) { Text("Discard",color = MaterialTheme.colorScheme.error) } },
        dismissButton = { TextButton(onClick = { discarding = false }) { Text("Keep editing") } })
}

@Composable private fun LaunchActionBar(progress: String,error: String,reason: String,busy: Boolean,onCreate: () -> Unit) {
    Surface(color = MaterialTheme.colorScheme.background) {
        Column(Modifier.fillMaxWidth().windowInsetsPadding(WindowInsets.navigationBars.union(WindowInsets.ime).only(WindowInsetsSides.Horizontal + WindowInsetsSides.Bottom))) {
            HorizontalDivider(color = MaterialTheme.colorScheme.outline)
            Column(Modifier.padding(horizontal = 16.dp,vertical = 12.dp),verticalArrangement = Arrangement.spacedBy(8.dp)) {
                if(error.isNotBlank()) Text(error,style = MaterialTheme.typography.bodySmall,color = Amber,modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite })
                if(reason.isNotBlank() && reason != error) Text(reason,style = MaterialTheme.typography.bodySmall,color = MaterialTheme.colorScheme.onSurfaceVariant)
                Button(onClick = onCreate,enabled = !busy && reason.isBlank(),modifier = Modifier.fillMaxWidth().heightIn(min = 52.dp)
                    .semantics { if(busy) liveRegion = LiveRegionMode.Polite }) {
                    if(busy) { CircularProgressIndicator(Modifier.size(18.dp),strokeWidth = 2.dp,color = LocalContentColor.current);Spacer(Modifier.width(10.dp)) }
                    Text(if(busy) progress.ifBlank { "Working…" } else "Create session")
                }
            }
        }
    }
}

@Composable private fun LaunchSection(title: String,content: @Composable ColumnScope.() -> Unit) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(title,style = MaterialTheme.typography.titleSmall,color = MaterialTheme.colorScheme.onSurfaceVariant,modifier = Modifier.semantics { heading() })
        content()
    }
}

@Composable private fun LaunchHint(text: String,modifier: Modifier = Modifier,color: Color = MaterialTheme.colorScheme.onSurfaceVariant) =
    Text(text,modifier,style = MaterialTheme.typography.bodySmall,color = color)

@Composable internal fun LaunchAccountList(choices:List<LaunchAccountChoice>,provider:String,account:String,enabled:Boolean,onChoose:(LaunchAccount)->Unit) {
    var now by remember { mutableDoubleStateOf(System.currentTimeMillis()/1000.0) }
    LaunchedEffect(Unit) { while(true) { delay(30_000);now=System.currentTimeMillis()/1000.0 } }
    val logoSize=with(LocalDensity.current) { MaterialTheme.typography.bodyLarge.lineHeight.toDp()+MaterialTheme.typography.bodySmall.lineHeight.toDp() }
    Surface(Modifier.fillMaxWidth(),shape = RoundedCornerShape(12.dp),color = MaterialTheme.colorScheme.surface,
        border = BorderStroke(1.dp,MaterialTheme.colorScheme.outline)) {
        Column(Modifier.heightIn(max = 280.dp).verticalScroll(rememberScrollState()).selectableGroup().alpha(if(enabled) 1f else .5f)) {
            choices.forEach { choice -> key(choice.key) {
                val selected = choice.account.provider == provider && choice.account.id == account
                val limits=choice.usage?.let { LaunchPresentation.accountLimits(it,now) } ?: choice.limits
                Surface(color = if(selected) MaterialTheme.colorScheme.primary.copy(alpha = .14f) else Color.Transparent) {
                    Row(Modifier.fillMaxWidth().heightIn(min = 64.dp).selectable(selected,enabled = enabled,role = Role.RadioButton) { onChoose(choice.account) }
                        .semantics { contentDescription = "${DesktopIcons.providerName(choice.account.provider)} ${choice.title}, $limits" }
                        .padding(horizontal = 14.dp,vertical = 10.dp),verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        Icon(DesktopIcons.providerLogo(choice.account.provider),DesktopIcons.providerName(choice.account.provider),Modifier.size(logoSize),
                            tint = DesktopIcons.providerColor(choice.account.provider,MaterialTheme.colorScheme.surface.luminance()<.5f))
                        Column(Modifier.weight(1f)) {
                            Text(choice.title,maxLines = 1,overflow = TextOverflow.Ellipsis,style = MaterialTheme.typography.bodyLarge,fontWeight = FontWeight.SemiBold)
                            LaunchAccountLimits(choice,now,limits)
                        }
                    }
                }
            } }
        }
    }
}

@Composable private fun LaunchAccountLimits(choice:LaunchAccountChoice,now:Double,limits:String) {
    val windows=LaunchPresentation.limitWindows(choice.usage,now).take(2)
    if(windows.isEmpty()) {
        Text(limits,maxLines=1,overflow=TextOverflow.Ellipsis,style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.onSurfaceVariant)
        return
    }
    val dark=MaterialTheme.colorScheme.surface.luminance()<.5f
    Row(Modifier.fillMaxWidth(),horizontalArrangement=Arrangement.spacedBy(10.dp)) {
        windows.forEach { window ->
            val pace=LaunchPresentation.limitPace(window,now)
            val color=when(pace.tone) {
                LaunchLimitTone.Normal -> if(dark) Color(0xFF72CDB2) else Color(0xFF237A62)
                LaunchLimitTone.Warning -> if(dark) Color(0xFFF0A35B) else Color(0xFFBD6519)
                LaunchLimitTone.Critical -> if(dark) Color(0xFFF07878) else Color(0xFFCE3D47)
            }
            Row(Modifier.weight(1f),horizontalArrangement=Arrangement.spacedBy(6.dp),verticalAlignment=Alignment.CenterVertically) {
                Canvas(Modifier.size(20.dp,10.dp)) {
                    val track=if(dark) Color(0xFF36434E) else Color(0xFFD7E0E6)
                    val top=if(pace.elapsed==null) 3.dp.toPx() else 1.dp.toPx()
                    val height=if(pace.elapsed==null) 4.dp.toPx() else 3.dp.toPx()
                    val radius=CornerRadius(height/2)
                    drawRoundRect(track,Offset(0f,top),Size(size.width,height),radius)
                    drawRoundRect(color,Offset(0f,top),Size(size.width*(pace.used/100).toFloat(),height),radius)
                    pace.elapsed?.let { elapsed ->
                        val bottom=6.dp.toPx();val rail=2.dp.toPx()
                        drawRoundRect(track,Offset(0f,bottom),Size(size.width,rail),CornerRadius(rail/2))
                        drawRoundRect(if(dark) Color(0xFF8395A5) else Color(0xFF7F90A1),Offset(0f,bottom),Size(size.width*elapsed.toFloat(),rail),CornerRadius(rail/2))
                    }
                }
                Text("${AccountPresentation.shortPeriod(window)} ${(pace.used+.5).toInt()}%",maxLines=1,softWrap=false,overflow=TextOverflow.Ellipsis,style=MaterialTheme.typography.bodySmall,color=color)
            }
        }
    }
}

@Composable private fun LaunchNotice(accent: Color?,content: @Composable ColumnScope.() -> Unit) {
    Surface(Modifier.fillMaxWidth(),color = MaterialTheme.colorScheme.surface,shape = RoundedCornerShape(12.dp),
        border = BorderStroke(1.dp,accent?.copy(alpha = .55f) ?: MaterialTheme.colorScheme.outline)) {
        Column(Modifier.padding(horizontal = 14.dp,vertical = 10.dp),verticalArrangement = Arrangement.spacedBy(4.dp),content = content)
    }
}

@Composable private fun PlannedFolder(action: SessionAction) {
    val planned = runCatching { JSONObject(action.arguments).string("destination") }.getOrDefault("")
    if(planned.isNotBlank()) SelectionContainer { Text(planned,style = MaterialTheme.typography.bodySmall,color = MaterialTheme.colorScheme.onSurfaceVariant) }
}

private fun projectColor(hex: String?,fallback: Color): Color = hex?.takeIf { it.isNotBlank() }
    ?.let { runCatching { Color(it.toColorInt()) }.getOrNull() } ?: fallback

/** One field style for every selector: 56dp, outlined, value with an optional supporting line. */
@Composable private fun LaunchField(label: String,value: String,enabled: Boolean,onClick: () -> Unit,modifier: Modifier = Modifier,supporting: String? = null,
    leading: (@Composable () -> Unit)? = null,trailing: ImageVector? = Icons.Default.ArrowDropDown) {
    Surface(onClick = onClick,enabled = enabled,shape = RoundedCornerShape(12.dp),color = MaterialTheme.colorScheme.surface,
        border = BorderStroke(1.dp,MaterialTheme.colorScheme.outline),
        modifier = modifier.fillMaxWidth().heightIn(min = 56.dp).semantics { contentDescription = listOfNotNull(label,value,supporting).joinToString(", ") }) {
        Row(Modifier.alpha(if(enabled) 1f else .5f).padding(horizontal = 14.dp,vertical = 8.dp),verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            leading?.invoke()
            Column(Modifier.weight(1f)) {
                Text(value,maxLines = 1,overflow = TextOverflow.Ellipsis,style = MaterialTheme.typography.bodyLarge)
                supporting?.takeIf { it.isNotBlank() }?.let { Text(it,maxLines = 1,overflow = TextOverflow.Ellipsis,style = MaterialTheme.typography.bodySmall,color = MaterialTheme.colorScheme.onSurfaceVariant) }
            }
            trailing?.let { Icon(it,null,tint = MaterialTheme.colorScheme.onSurfaceVariant) }
        }
    }
}

@Composable private fun LaunchMenuField(label: String,value: String,enabled: Boolean,choices: List<Pair<String,String>>,selected: String,onChoose: (String) -> Unit,
    modifier: Modifier = Modifier,supporting: String? = null,leading: (@Composable () -> Unit)? = null,choiceLeading: (@Composable (String) -> Unit)? = null,
    choiceSupporting: ((String) -> String)? = null,choiceSupportingMaxLines:Int = 1,onEmpty: (() -> Unit)? = null) {
    var expanded by remember { mutableStateOf(false) }
    Box(modifier) {
        LaunchField(label,value,enabled && (choices.isNotEmpty() || onEmpty != null),{ if(choices.isEmpty()) onEmpty?.invoke() else expanded = true },
            supporting = supporting,leading = leading)
        DropdownMenu(expanded && enabled,onDismissRequest = { expanded = false }) {
            choices.forEach { (id,title) -> DropdownMenuItem(text = {
                Column {
                    Text(title,fontWeight = if(id == selected) FontWeight.SemiBold else FontWeight.Normal,maxLines = 1,overflow = TextOverflow.Ellipsis)
                    choiceSupporting?.invoke(id)?.takeIf { it.isNotBlank() }?.let { Text(it,style = MaterialTheme.typography.bodySmall,color = MaterialTheme.colorScheme.onSurfaceVariant,maxLines = choiceSupportingMaxLines,overflow = TextOverflow.Ellipsis) }
                }
            },leadingIcon = choiceLeading?.let { icon -> { icon(id) } },
                trailingIcon = if(id == selected) { { Icon(Icons.Default.Check,"Selected",tint = MaterialTheme.colorScheme.primary) } } else null,
                onClick = { expanded = false;onChoose(id) }) }
        }
    }
}

/** Searchable, full-height project list in catalog order; it opens at the current selection. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable private fun ProjectPickerDialog(projects: List<LaunchProject>,selected: String,onChoose: (String) -> Unit,onDismiss: () -> Unit) {
    var query by rememberSaveable { mutableStateOf("") }
    val visible = remember(projects,query) { LaunchPresentation.filterProjects(projects,query) }
    val list = rememberLazyListState(initialFirstVisibleItemIndex = (projects.indexOfFirst { it.id == selected } - 2).coerceAtLeast(0))
    LaunchedEffect(query) { if(query.isNotEmpty()) list.scrollToItem(0) }
    Dialog(onDismissRequest = onDismiss,properties = DialogProperties(usePlatformDefaultWidth = false,decorFitsSystemWindows = false)) {
        Surface(Modifier.fillMaxSize(),color = MaterialTheme.colorScheme.background) {
            Scaffold(containerColor = MaterialTheme.colorScheme.background,topBar = {
                TopAppBar(title = { Text("Choose project") },navigationIcon = { IconButton(onClick = onDismiss) { Icon(Icons.AutoMirrored.Filled.ArrowBack,"Back") } },
                    colors = TopAppBarDefaults.topAppBarColors(containerColor = MaterialTheme.colorScheme.background))
            }) { padding ->
                Column(Modifier.padding(padding).consumeWindowInsets(padding).fillMaxSize().imePadding()) {
                    OutlinedTextField(query,{ query = it },Modifier.fillMaxWidth().padding(horizontal = 16.dp,vertical = 8.dp),singleLine = true,
                        placeholder = { Text("Search projects and folders") },leadingIcon = { Icon(Icons.Default.Search,null) },
                        trailingIcon = if(query.isNotEmpty()) { { IconButton(onClick = { query = "" }) { Icon(Icons.Default.Close,"Clear search") } } } else null,
                        shape = RoundedCornerShape(14.dp),keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search))
                    if(visible.isEmpty()) Text(if(projects.isEmpty()) "No projects are available on this computer." else "No projects match",
                        Modifier.padding(horizontal = 16.dp,vertical = 24.dp),color = MaterialTheme.colorScheme.onSurfaceVariant)
                    LazyColumn(Modifier.weight(1f),state = list,contentPadding = PaddingValues(bottom = 16.dp)) {
                        items(visible,key = { it.id }) { project ->
                            val chosen = project.id == selected
                            Row(Modifier.fillMaxWidth().heightIn(min = 64.dp).selectable(chosen,role = Role.RadioButton) { onChoose(project.id) }
                                .padding(horizontal = 16.dp,vertical = 10.dp),verticalAlignment = Alignment.CenterVertically,horizontalArrangement = Arrangement.spacedBy(14.dp)) {
                                Icon(DesktopIcons.Projects,null,tint = projectColor(project.color,MaterialTheme.colorScheme.primary))
                                Column(Modifier.weight(1f)) {
                                    Text(project.name,maxLines = 1,overflow = TextOverflow.Ellipsis,fontWeight = if(chosen) FontWeight.SemiBold else FontWeight.Normal)
                                    Text(LaunchPresentation.folderSummary(project),maxLines = 1,overflow = TextOverflow.Ellipsis,
                                        style = MaterialTheme.typography.bodySmall,color = MaterialTheme.colorScheme.onSurfaceVariant)
                                }
                                if(chosen) Icon(Icons.Default.Check,"Selected",tint = MaterialTheme.colorScheme.primary)
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable private fun WorktreeChooserDialog(entries: List<LaunchWorktree>,selected: String,onChoose: (String) -> Unit,onDismiss: () -> Unit) {
    ZerusAlertDialog(onDismissRequest = onDismiss,title = { Text("Choose an existing worktree") },text = {
        Column(Modifier.heightIn(max = 420.dp).verticalScroll(rememberScrollState())) {
            entries.forEach { entry ->
                val chosen = entry.path == selected
                Row(Modifier.fillMaxWidth().heightIn(min = 56.dp).selectable(chosen,role = Role.RadioButton) { onChoose(entry.path) }.padding(vertical = 8.dp),
                    verticalAlignment = Alignment.CenterVertically,horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    Icon(DesktopIcons.GitBranch,null,Modifier.size(20.dp),tint = MaterialTheme.colorScheme.onSurfaceVariant)
                    Column(Modifier.weight(1f)) {
                        Text(entry.branch.ifBlank { "detached HEAD" },maxLines = 1,overflow = TextOverflow.Ellipsis,fontWeight = if(chosen) FontWeight.SemiBold else FontWeight.Normal)
                        Text(if(entry.kind == "main") "Main checkout · ${entry.path}" else entry.path,style = MaterialTheme.typography.bodySmall,color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                    if(chosen) Icon(Icons.Default.Check,"Selected",tint = MaterialTheme.colorScheme.primary)
                }
            }
        }
    },confirmButton = { TextButton(onClick = onDismiss) { Text("Close") } })
}

@Composable internal fun LaunchFolderDialog(model: ZerusViewModel,machine: MachineKey,onChoose: (String) -> Unit,onDismiss: () -> Unit) {
    val raw = model.launchDirectoryState
    val path = raw?.string("path").orEmpty()
    val parent = raw?.string("parent").orEmpty()
    val home = raw?.string("home").orEmpty()
    val form = model.launchFormId
    var typedPath by rememberSaveable(machine,form) { mutableStateOf(path) }
    var lastFolder by rememberSaveable(machine,form) { mutableStateOf(path) }
    var creatingPath by rememberSaveable(machine,form) { mutableStateOf<String?>(null) }
    var observedCreation by rememberSaveable(machine,form) { mutableStateOf("") }
    LaunchedEffect(path) { if(path.isNotBlank()) { if(typedPath.isBlank()) typedPath=path;lastFolder=path } }
    LaunchedEffect(model.launchCreatedDirectory) {
        val created=model.launchCreatedDirectory
        if(created.isNotBlank() && created!=observedCreation) { observedCreation=created;typedPath=created;creatingPath=null }
    }
    val folders = remember(raw) { LaunchPresentation.folders(raw) }
    val busy = model.launchBusy || model.launchDirectoryLoading
    val pending = DirectoryCreation.pending(model.sessionActions,machine)
    val createReason = model.directoryCreateReason(machine)
    val computer = model.machines.find { it.connectionId==machine.connectionId && it.id==machine.computerId }?.name ?: machine.computerId
    fun dismiss() { if(!model.launchBusy) onDismiss() }
    ZerusAlertDialog(onDismissRequest = ::dismiss,title = { Text("Choose folder on $computer") },text = {
        Column(Modifier.heightIn(max = 480.dp).verticalScroll(rememberScrollState()),verticalArrangement = Arrangement.spacedBy(8.dp)) {
            if(model.launchDirectoryLoading || model.directoryCreateSubmitting) LinearProgressIndicator(Modifier.fillMaxWidth())
            SelectionContainer { Text(path.ifBlank { lastFolder.ifBlank { "Loading native folders…" } },style = MaterialTheme.typography.bodySmall) }
            FlowRow {
                TextButton(onClick = { model.browseLaunchDirectory(machine,parent) },enabled = parent.isNotBlank() && !busy,modifier = Modifier.heightIn(min=48.dp)) { Text("Up") }
                TextButton(onClick = { model.browseLaunchDirectory(machine,home.ifBlank { "~" }) },enabled = !busy,modifier = Modifier.heightIn(min=48.dp)) { Text("Home") }
                TextButton(onClick = { creatingPath=DirectoryCreation.child(lastFolder,"New folder") },enabled = LaunchPresentation.absoluteFolder(lastFolder) && !busy && createReason.isBlank(),modifier = Modifier.heightIn(min=48.dp)) {
                    Icon(Icons.Default.Add,null,Modifier.size(18.dp));Text("New folder",Modifier.padding(start=4.dp))
                }
            }
            OutlinedTextField(typedPath,{ typedPath = it },Modifier.fillMaxWidth(),label = { Text("Absolute folder path") },singleLine = true,readOnly = busy)
            FlowRow {
                TextButton(onClick = { model.browseLaunchDirectory(machine,typedPath) },enabled = LaunchPresentation.absoluteFolder(typedPath) && !busy,modifier = Modifier.heightIn(min=48.dp)) { Text("Browse") }
                if(typedPath.isNotBlank() && typedPath!=path && folders.none { it.path==typedPath })
                    TextButton(onClick = { creatingPath=typedPath },enabled = DirectoryCreation.pathError(typedPath).isBlank() && !busy && createReason.isBlank(),modifier = Modifier.heightIn(min=48.dp)) { Text("Create this path") }
            }
            pending?.let { action ->
                Text(DirectoryCreation.uncertain,style=MaterialTheme.typography.bodySmall)
                SelectionContainer { Text(runCatching { JSONObject(action.arguments).string("path") }.getOrDefault(""),style=MaterialTheme.typography.bodySmall) }
                TextButton(onClick = { model.checkLaunchDirectoryCreation(machine,action) },enabled = !busy,modifier = Modifier.heightIn(min=48.dp)) { Text("Check original folder result") }
            }
            if(pending==null && createReason.isNotBlank()) Text(createReason,style=MaterialTheme.typography.bodySmall)
            folders.forEach { folder -> TextButton(onClick = { model.browseLaunchDirectory(machine,folder.path) },enabled = !busy,modifier = Modifier.fillMaxWidth().heightIn(min=48.dp)) {
                Row(Modifier.fillMaxWidth(),verticalAlignment = Alignment.CenterVertically) { Icon(Icons.Default.Folder,null);Text(folder.name,Modifier.padding(start = 8.dp)) }
            } }
            if(raw?.optBoolean("truncated") == true) Text("Folder listing is incomplete. Browse an exact path to reach another folder.",style = MaterialTheme.typography.bodySmall)
            if(model.launchError.isNotBlank()) Text(model.launchError,color = MaterialTheme.colorScheme.secondary,style = MaterialTheme.typography.bodySmall)
        }
    },confirmButton = { TextButton(onClick = { onChoose(path) },enabled = LaunchPresentation.absoluteFolder(path) && !busy,modifier = Modifier.heightIn(min=48.dp)) { Text("Use this folder") } },
        dismissButton = { TextButton(onClick = ::dismiss,enabled = !model.launchBusy,modifier = Modifier.heightIn(min=48.dp)) { Text("Cancel") } })
    creatingPath?.let { initial -> CreateFolderDialog(model,machine,computer,initial) { if(!model.launchBusy) creatingPath=null } }
}

@Composable private fun CreateFolderDialog(model:ZerusViewModel,machine:MachineKey,computer:String,initial:String,onDismiss:()->Unit) {
    var path by rememberSaveable(machine,model.launchFormId,initial) { mutableStateOf(initial) }
    val busy=model.directoryCreateSubmitting
    val problem=DirectoryCreation.pathError(path)
    ZerusAlertDialog(onDismissRequest = { if(!busy) onDismiss() },properties = DialogProperties(dismissOnClickOutside=false),
        title = { Text("Create folder") },text = {
            Column(Modifier.heightIn(max=400.dp).verticalScroll(rememberScrollState()),verticalArrangement=Arrangement.spacedBy(8.dp)) {
                Text("Create a folder on $computer. Its parent must already exist.",style=MaterialTheme.typography.bodyMedium)
                OutlinedTextField(path,{ path=it },Modifier.fillMaxWidth(),label={ Text("New folder path") },singleLine=true,readOnly=busy)
                if(problem.isNotBlank()) Text(problem,style=MaterialTheme.typography.bodySmall)
                if(model.launchError.isNotBlank()) Text(model.launchError,style=MaterialTheme.typography.bodySmall,color=MaterialTheme.colorScheme.secondary)
                if(busy) LinearProgressIndicator(Modifier.fillMaxWidth())
            }
        },confirmButton = { TextButton(onClick = { model.createLaunchDirectory(machine,path) },enabled = !busy && problem.isBlank() && model.directoryCreateReason(machine).isBlank(),modifier=Modifier.heightIn(min=48.dp)) {
            Text(if(busy) "Creating folder…" else "Create folder")
        } },dismissButton = { TextButton(onClick = onDismiss,enabled = !busy,modifier=Modifier.heightIn(min=48.dp)) { Text("Cancel") } })
}

/** Desktop Create manually…: a new branch and folder; the branch suggestion follows the session name. */
@Composable private fun CreateWorktreeDialog(model:ZerusViewModel,machine:MachineKey,evidence:JSONObject,suggestedBranch:String,onDismiss:()->Unit) {
    val repository = evidence.string("common_dir")
    var branch by rememberSaveable(repository) { mutableStateOf(suggestedBranch) }
    var base by rememberSaveable(repository) { mutableStateOf("HEAD") }
    var customPath by rememberSaveable(repository) { mutableStateOf(false) }
    var destination by rememberSaveable(repository) { mutableStateOf(WorktreePresentation.destination(evidence,suggestedBranch)) }
    val problem = WorktreePresentation.createError(branch,base,destination)
    val submitting = model.worktreeSubmitting
    ZerusAlertDialog(onDismissRequest = onDismiss,properties = DialogProperties(dismissOnClickOutside = false),title = { Text("Create worktree") },text = {
        Column(Modifier.heightIn(max = 460.dp).verticalScroll(rememberScrollState()),verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Creates a new branch and working folder on this computer. It remains there if you cancel or cannot start the session.",style = MaterialTheme.typography.bodySmall)
            SelectionContainer { Text(evidence.string("path"),style = MaterialTheme.typography.bodySmall,color = MaterialTheme.colorScheme.onSurfaceVariant) }
            OutlinedTextField(branch,{ branch = it;if(!customPath) destination = WorktreePresentation.destination(evidence,it) },Modifier.fillMaxWidth(),
                label = { Text("New branch") },singleLine = true,readOnly = submitting)
            OutlinedTextField(base,{ base = it },Modifier.fillMaxWidth(),label = { Text("Start from") },singleLine = true,readOnly = submitting)
            OutlinedTextField(destination,{ destination = it;customPath = true },Modifier.fillMaxWidth(),label = { Text("New folder") },singleLine = true,readOnly = submitting)
            if(problem.isNotBlank()) Text(problem,style = MaterialTheme.typography.bodySmall)
            if(model.launchError.isNotBlank()) Text(model.launchError,style = MaterialTheme.typography.bodySmall,color = MaterialTheme.colorScheme.secondary)
            if(submitting) LinearProgressIndicator(Modifier.fillMaxWidth())
        }
    },confirmButton = { TextButton(onClick = { model.createLaunchWorktree(machine,evidence,branch,base,destination) },
        enabled = !submitting && problem.isBlank() && model.worktreeReason(machine).isBlank()) { Text(if(submitting) "Creating…" else "Create worktree") } },
        dismissButton = { TextButton(onClick = onDismiss,enabled = !submitting) { Text("Cancel") } })
}
