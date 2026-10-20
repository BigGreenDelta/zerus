//! The task replica for the mobile apps through UniFFI: the typed operations
//! of `zerus-replica`, on documents below a folder the app chooses in its
//! private files. The person acts on the phone. Sends and cancels run on the
//! session's computer, over the relay exchange. See "The replica crate" in
//! docs/superpowers/specs/2026-10-18-task-replication-loro-design.md.
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use zerus_replica as replica;

uniffi::setup_scaffolding!();

/// A rule refused the change and nothing changed (show the message), or
/// storage failed.
#[derive(Debug, uniffi::Error)]
#[uniffi(flat_error)]
pub enum ReplicaError {
    Refused(String),
    Failed(String),
}

impl std::fmt::Display for ReplicaError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Refused(message) | Self::Failed(message) => f.write_str(message),
        }
    }
}

impl std::error::Error for ReplicaError {}

impl From<replica::Error> for ReplicaError {
    fn from(error: replica::Error) -> Self {
        match error.kind() {
            replica::ErrorKind::Refused => Self::Refused(error.to_string()),
            replica::ErrorKind::Failed => Self::Failed(error.to_string()),
        }
    }
}

type Result<T> = std::result::Result<T, ReplicaError>;

/// How long a call waits for a project another caller holds, such as an exchange.
const LOCK_WAIT: Duration = Duration::from_secs(5);

fn now() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_millis() as i64)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, uniffi::Enum)]
pub enum TaskStage {
    Backlog,
    Todo,
    Active,
    Review,
    Done,
}

impl From<replica::Stage> for TaskStage {
    fn from(stage: replica::Stage) -> Self {
        match stage {
            replica::Stage::Backlog => Self::Backlog,
            replica::Stage::Todo => Self::Todo,
            replica::Stage::Active => Self::Active,
            replica::Stage::Review => Self::Review,
            replica::Stage::Done => Self::Done,
        }
    }
}

impl From<TaskStage> for replica::Stage {
    fn from(stage: TaskStage) -> Self {
        match stage {
            TaskStage::Backlog => Self::Backlog,
            TaskStage::Todo => Self::Todo,
            TaskStage::Active => Self::Active,
            TaskStage::Review => Self::Review,
            TaskStage::Done => Self::Done,
        }
    }
}

/// A session on a computer: the computer's swarm identity and the session name.
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct SessionRef {
    pub machine: String,
    pub session: String,
}

impl From<replica::SessionKey> for SessionRef {
    fn from(key: replica::SessionKey) -> Self {
        Self {
            machine: key.machine,
            session: key.session,
        }
    }
}

impl From<SessionRef> for replica::SessionKey {
    fn from(key: SessionRef) -> Self {
        Self {
            machine: key.machine,
            session: key.session,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, uniffi::Enum)]
/// Any session, or one; `AnySession`, since Kotlin's `Any` is taken.
pub enum TaskTarget {
    AnySession,
    Session { session: SessionRef },
}

impl From<replica::Target> for TaskTarget {
    fn from(target: replica::Target) -> Self {
        match target {
            replica::Target::Any => Self::AnySession,
            replica::Target::Session(key) => Self::Session {
                session: key.into(),
            },
        }
    }
}

impl From<TaskTarget> for replica::Target {
    fn from(target: TaskTarget) -> Self {
        match target {
            TaskTarget::AnySession => Self::Any,
            TaskTarget::Session { session } => Self::Session(session.into()),
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, uniffi::Enum)]
pub enum TaskActor {
    Person,
    Agent { session: SessionRef },
}

impl From<replica::Actor> for TaskActor {
    fn from(actor: replica::Actor) -> Self {
        match actor {
            replica::Actor::Person => Self::Person,
            replica::Actor::Agent(key) => Self::Agent {
                session: key.into(),
            },
        }
    }
}

/// One entry of a task's history.
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct TaskLogEntry {
    pub kind: String,
    pub actor: TaskActor,
    pub at: i64,
    pub text: String,
    pub session: Option<SessionRef>,
    pub request_id: Option<String>,
}

impl From<replica::LogEntry> for TaskLogEntry {
    fn from(entry: replica::LogEntry) -> Self {
        Self {
            kind: entry.kind,
            actor: entry.actor.into(),
            at: entry.at,
            text: entry.text,
            session: entry.session.map(Into::into),
            request_id: entry.request_id,
        }
    }
}

/// A task as the apps read it; times are Unix milliseconds.
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct TaskInfo {
    pub uuid: String,
    pub id: String,
    pub project: String,
    pub schema: i64,
    pub writable: bool,
    pub created_at: i64,
    pub stage_at: i64,
    pub author: TaskActor,
    pub stage: TaskStage,
    pub target: TaskTarget,
    pub rank: String,
    pub session: Option<SessionRef>,
    pub summary: String,
    pub cancelled_at: Option<i64>,
    pub cancel_reason: String,
    pub text: String,
    pub log: Vec<TaskLogEntry>,
}

impl From<replica::TaskView> for TaskInfo {
    fn from(view: replica::TaskView) -> Self {
        Self {
            uuid: view.uuid,
            id: view.id,
            project: view.project,
            schema: view.schema,
            writable: view.writable,
            created_at: view.created_at,
            stage_at: view.stage_at,
            author: view.author.into(),
            stage: view.stage.into(),
            target: view.target.into(),
            rank: view.rank,
            session: view.session.map(Into::into),
            summary: view.summary,
            cancelled_at: view.cancelled_at,
            cancel_reason: view.cancel_reason,
            text: view.text,
            log: view.log.into_iter().map(Into::into).collect(),
        }
    }
}

/// A task file that failed to load; it stays on disk and never blocks the others.
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct DamagedTask {
    pub uuid: String,
    pub error: String,
}

impl From<replica::Damaged> for DamagedTask {
    fn from(damaged: replica::Damaged) -> Self {
        Self {
            uuid: damaged.uuid,
            error: damaged.error,
        }
    }
}

/// Every task of the project in stage order, each stage in its order.
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct TaskListing {
    pub tasks: Vec<TaskInfo>,
    pub damaged: Vec<DamagedTask>,
}

/// Where a task goes in the order of its stage.
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Enum)]
pub enum TaskPosition {
    Top,
    End,
    /// From the top, zero-based; beyond the end means the end.
    Index {
        index: u32,
    },
    /// Before or after the task with this short ID or UUID.
    Before {
        reference: String,
    },
    After {
        reference: String,
    },
}

impl From<TaskPosition> for replica::Position {
    fn from(position: TaskPosition) -> Self {
        match position {
            TaskPosition::Top => Self::Top,
            TaskPosition::End => Self::End,
            TaskPosition::Index { index } => Self::Index(index as usize),
            TaskPosition::Before { reference } => Self::Before(reference),
            TaskPosition::After { reference } => Self::After(reference),
        }
    }
}

/// The project's task settings.
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct TaskSettings {
    pub task_template: String,
    pub cancel_template: String,
    /// Agents may start and cancel tasks in other sessions.
    pub agents_other: bool,
    /// Tasks agents may start per hour in the project; 0 is no limit.
    pub agent_limit: i64,
}

impl From<replica::Settings> for TaskSettings {
    fn from(settings: replica::Settings) -> Self {
        Self {
            task_template: settings.task_template,
            cancel_template: settings.cancel_template,
            agents_other: settings.agents_other,
            agent_limit: settings.agent_limit,
        }
    }
}

impl From<TaskSettings> for replica::Settings {
    fn from(settings: TaskSettings) -> Self {
        Self {
            task_template: settings.task_template,
            cancel_template: settings.cancel_template,
            agents_other: settings.agents_other,
            agent_limit: settings.agent_limit,
        }
    }
}

/// Loro updates for one document: `task` is its UUID, or none for the project document.
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct DocumentUpdate {
    pub task: Option<String>,
    pub bytes: Vec<u8>,
}

impl From<replica::Update> for DocumentUpdate {
    fn from(update: replica::Update) -> Self {
        let task = match update.document {
            replica::Document::Project => None,
            replica::Document::Task(uuid) => Some(uuid),
        };
        Self {
            task,
            bytes: update.bytes,
        }
    }
}

impl From<DocumentUpdate> for replica::Update {
    fn from(update: DocumentUpdate) -> Self {
        let document = match update.task {
            None => replica::Document::Project,
            Some(uuid) => replica::Document::Task(uuid),
        };
        Self {
            document,
            bytes: update.bytes,
        }
    }
}

/// A page of what another replica lacks; `more` when the next page has more.
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct UpdatePage {
    pub updates: Vec<DocumentUpdate>,
    pub more: bool,
}

/// What an import changed, and the documents it refused.
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct ImportResult {
    pub changed: u32,
    pub refused: Vec<DamagedTask>,
}

/// The tasks of one project below `root`. Each call reads and writes files and
/// opens the project under its lock, waiting up to five seconds while another
/// caller holds it (then it fails with "busy"); call it off the main thread,
/// on `Dispatchers.IO`. The lock is released on return.
#[derive(uniffi::Object)]
pub struct TaskStore {
    root: PathBuf,
    project: String,
    project_name: String,
}

impl TaskStore {
    fn store(&self) -> Result<replica::ProjectStore> {
        replica::ProjectStore::open_within(&self.root, &self.project, LOCK_WAIT)?
            .ok_or_else(|| ReplicaError::Failed("the project's tasks are busy; try again".into()))
    }

    /// Runs `change` on the task `reference` and saves it.
    fn change(
        &self,
        reference: &str,
        change: impl FnOnce(&replica::ProjectStore, &mut replica::TaskDoc) -> replica::Result<()>,
    ) -> Result<TaskInfo> {
        let store = self.store()?;
        let mut task = store.find(reference)?;
        change(&store, &mut task)?;
        store.save(&mut task)?;
        Ok(task.view()?.into())
    }
}

#[uniffi::export]
impl TaskStore {
    /// `project` is the project's catalog ID and `project_name` its current
    /// name, which task prompts carry.
    #[uniffi::constructor]
    pub fn new(root: String, project: String, project_name: String) -> Arc<Self> {
        Arc::new(Self {
            root: PathBuf::from(root),
            project,
            project_name,
        })
    }

    pub fn list(&self) -> Result<TaskListing> {
        let (tasks, damaged) = self.store()?.listing()?;
        Ok(TaskListing {
            tasks: tasks
                .iter()
                .map(|task| task.view().map(TaskInfo::from))
                .collect::<replica::Result<_>>()?,
            damaged: damaged.into_iter().map(Into::into).collect(),
        })
    }

    /// The task with this short ID or UUID.
    pub fn get(&self, reference: String) -> Result<TaskInfo> {
        Ok(self.store()?.find(&reference)?.view()?.into())
    }

    /// Files a task by the person, to To do or Backlog.
    pub fn add(
        &self,
        text: String,
        stage: TaskStage,
        target: TaskTarget,
        position: TaskPosition,
    ) -> Result<TaskInfo> {
        let store = self.store()?;
        let mut task = store.new_task(
            &text,
            stage.into(),
            target.into(),
            replica::Actor::Person,
            &position.into(),
            &self.project_name,
            now(),
        )?;
        store.save(&mut task)?;
        Ok(task.view()?.into())
    }

    pub fn edit_text(&self, reference: String, text: String) -> Result<TaskInfo> {
        let name = self.project_name.clone();
        self.change(&reference, |store, task| {
            store.edit_task(task, &text, &replica::Actor::Person, &name, now())
        })
    }

    pub fn set_stage(&self, reference: String, stage: TaskStage) -> Result<TaskInfo> {
        self.change(&reference, |store, task| {
            store.change_stage(task, stage.into(), &replica::Actor::Person, now())
        })
    }

    pub fn set_target(&self, reference: String, target: TaskTarget) -> Result<TaskInfo> {
        self.change(&reference, |_, task| {
            task.set_target(&target.into(), &replica::Actor::Person, now())
        })
    }

    pub fn move_task(&self, reference: String, position: TaskPosition) -> Result<TaskInfo> {
        self.change(&reference, |store, task| {
            store.reorder(task, &position.into())
        })
    }

    pub fn comment(&self, reference: String, text: String) -> Result<TaskInfo> {
        self.change(&reference, |_, task| {
            task.comment(&text, &replica::Actor::Person, now())
        })
    }

    pub fn settings(&self) -> Result<TaskSettings> {
        Ok(self.store()?.project()?.settings().into())
    }

    /// Saves every field of `settings`.
    pub fn set_settings(&self, settings: TaskSettings) -> Result<TaskSettings> {
        let store = self.store()?;
        let mut doc = store.project()?;
        doc.set_settings(&settings.into())?;
        store.save_project(&doc)?;
        Ok(doc.settings().into())
    }

    /// What this replica holds, as JSON in the swarm's inventory form.
    pub fn inventory(&self) -> Result<String> {
        let (inventory, _) = self.store()?.inventory()?;
        serde_json::to_string(&inventory).map_err(|e| ReplicaError::Failed(e.to_string()))
    }

    /// A page of at most `limit` bytes of what the replica whose inventory is
    /// `theirs` lacks.
    pub fn updates_for(&self, theirs: String, limit: u64) -> Result<UpdatePage> {
        let theirs: replica::Inventory = serde_json::from_str(&theirs)
            .map_err(|e| ReplicaError::Refused(format!("invalid inventory: {e}")))?;
        let store = self.store()?;
        let (ours, _) = store.inventory()?;
        let (updates, more) =
            store.updates_for(&ours, &theirs, usize::try_from(limit).unwrap_or(usize::MAX))?;
        Ok(UpdatePage {
            updates: updates.into_iter().map(Into::into).collect(),
            more,
        })
    }

    /// The digest of the project document and every task here that is not
    /// Done, as a computer's heartbeat names its own.
    pub fn live_digest(&self) -> Result<String> {
        Ok(self.store()?.live_inventory()?.0.digest())
    }

    /// A new exchange of this project's tasks with one computer.
    pub fn exchange(self: Arc<Self>) -> Arc<TaskExchange> {
        Arc::new(TaskExchange {
            store: self,
            state: Mutex::new(replica::phone::Exchange::new()),
        })
    }

    /// An exchange that also fetches these tasks, which the phone does not
    /// hold (a Done task the person opens); each then stays with the phone.
    pub fn exchange_fetching(self: Arc<Self>, tasks: Vec<String>) -> Arc<TaskExchange> {
        let mut exchange = replica::phone::Exchange::new();
        exchange.fetch(tasks);
        Arc::new(TaskExchange {
            store: self,
            state: Mutex::new(exchange),
        })
    }

    /// The computer's side of one request, as hgs answers it: tests play a
    /// computer with it.
    pub fn answer_exchange(&self, request: String) -> Result<String> {
        let request: replica::phone::Request = serde_json::from_str(&request)
            .map_err(|e| ReplicaError::Refused(format!("invalid exchange request: {e}")))?;
        let answer = replica::phone::answer(&self.store()?, request)?;
        serde_json::to_string(&answer).map_err(|e| ReplicaError::Failed(e.to_string()))
    }

    pub fn import_updates(&self, updates: Vec<DocumentUpdate>) -> Result<ImportResult> {
        let imported = self
            .store()?
            .import_updates(updates.into_iter().map(Into::into).collect())?;
        Ok(ImportResult {
            changed: u32::try_from(imported.changed).unwrap_or(u32::MAX),
            refused: imported.refused.into_iter().map(Into::into).collect(),
        })
    }
}

/// What one answer did on the phone.
#[derive(Clone, Debug, PartialEq, Eq, uniffi::Record)]
pub struct ExchangeProgress {
    /// Documents the computer's updates changed here.
    pub changed: u32,
    /// The computer's updates this replica refused.
    pub refused_here: Vec<DamagedTask>,
    /// This replica's updates the computer refused; they are not sent again.
    pub refused_there: Vec<DamagedTask>,
    /// This replica's documents too large for a request ("project" or a task UUID).
    pub unsent: Vec<String>,
    /// The exchange has nothing left to do.
    pub finished: bool,
    /// The computer's live digest after this answer.
    pub digest: String,
}

/// The phone's exchange of one project's tasks with one computer, through
/// `task_exchange` requests the app carries to the relay: `next_request`
/// gives each payload, and `accept` takes the receipt's `result`. Each call
/// opens the project as the other methods do.
#[derive(uniffi::Object)]
pub struct TaskExchange {
    store: Arc<TaskStore>,
    state: Mutex<replica::phone::Exchange>,
}

impl TaskExchange {
    fn state(&self) -> std::sync::MutexGuard<'_, replica::phone::Exchange> {
        // A panic while held leaves nothing half-written on disk: go on.
        self.state
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }
}

#[uniffi::export]
impl TaskExchange {
    /// The next `task_exchange` payload, as JSON, or none once it is done.
    pub fn next_request(&self) -> Result<Option<String>> {
        let store = self.store.store()?;
        let Some(request) = self.state().next_request(&store)? else {
            return Ok(None);
        };
        serde_json::to_string(&request)
            .map(Some)
            .map_err(|e| ReplicaError::Failed(e.to_string()))
    }

    /// The computer's answer to the last request: the receipt's `result`, as JSON.
    pub fn accept(&self, answer: String) -> Result<ExchangeProgress> {
        let answer: replica::phone::Answer = serde_json::from_str(&answer)
            .map_err(|e| ReplicaError::Refused(format!("invalid exchange answer: {e}")))?;
        let store = self.store.store()?;
        let progress = self.state().accept(&store, answer)?;
        Ok(ExchangeProgress {
            changed: u32::try_from(progress.changed).unwrap_or(u32::MAX),
            refused_here: progress.refused_here.into_iter().map(Into::into).collect(),
            refused_there: progress.refused_there.into_iter().map(Into::into).collect(),
            unsent: progress.unsent,
            finished: progress.finished,
            digest: progress.digest,
        })
    }
}
