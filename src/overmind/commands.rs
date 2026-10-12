//! Overmind commands. A write runs in one transaction: replay its receipt,
//! fold the current state, prepare events, prove each by folding, append.
use super::blobs;
use super::fold::{fold, fold_many, project_workflow, Attachment, Task};
use super::model::*;
use super::rank;
use super::refs;
use super::store::{Filter, Store, Tx};
use super::workflow::{Action, Role, Workflow};
use anyhow::Result;
use base64::Engine;
use serde::de::DeserializeOwned;
use serde::Deserialize;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::path::PathBuf;

pub const MAX_PROMPT: usize = 64 * 1024;
pub const MAX_TITLE: usize = 200;
pub const MAX_FIELD: usize = 64 * 1024;
pub const MAX_ATTACHMENTS: usize = 8;
pub const MAX_ATTACHMENT: usize = 10 * 1024 * 1024;
pub const MAX_ATTACHMENTS_TOTAL: usize = 20 * 1024 * 1024;
const MAX_LABELS: usize = 32;
const RESERVED_LABELS: [&str; 4] = ["reviewed", "review-failed", "epic", "epic-ready"];

pub struct Ctx<'a> {
    pub store: &'a mut Store,
    pub actor: Actor,
    /// Working directory of the calling agent session, for evidence checks.
    pub cwd: Option<PathBuf>,
}

pub fn refuse<T>(code: &str, message: impl Into<String>) -> Result<T> {
    Err(Refusal::new(code, message).into())
}

pub fn parse<T: DeserializeOwned>(request: &Value) -> Result<T> {
    serde_json::from_value(request.clone())
        .map_err(|e| Refusal::new("invalid_request", format!("invalid request: {e}")).into())
}

pub fn human_only(actor: &Actor) -> Result<()> {
    if actor.kind != ActorKind::Human {
        return refuse("human_only", "only a person can do this");
    }
    Ok(())
}

/// Turns CRLF and lone CR into LF so pasted text from any platform is accepted.
pub fn normalize(text: &str) -> String {
    text.replace("\r\n", "\n").replace('\r', "\n")
}

pub fn check_text(name: &str, value: &str, max: usize, multiline: bool) -> Result<(), Refusal> {
    if value.len() > max {
        return Err(Refusal::field("too_long", name, "invalid", format!("`{name}` is longer than {max} bytes")));
    }
    let allowed = |c: char| !c.is_control() || (multiline && (c == '\n' || c == '\t'));
    if !value.chars().all(allowed) {
        return Err(Refusal::field("invalid_field", name, "invalid", format!("`{name}` contains control characters")));
    }
    Ok(())
}

fn check_label(label: &str) -> Result<(), Refusal> {
    let mut chars = label.chars();
    let first = chars.next().is_some_and(|c| c.is_ascii_lowercase() || c.is_ascii_digit());
    let rest = chars.all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || ":_./-".contains(c));
    if !first || !rest || label.len() > 64 {
        return Err(Refusal::field("invalid_field", "labels", "invalid", format!("invalid label {label:?}")));
    }
    Ok(())
}

fn check_reserved(label: &str) -> Result<(), Refusal> {
    if RESERVED_LABELS.contains(&label) {
        return Err(Refusal::new("reserved_label", format!("`{label}` is set by workflow actions")));
    }
    Ok(())
}

/// A session reference names a machine by UUID (empty for this machine) and a session.
fn check_session(field: &str, session: &SessionRef) -> Result<(), Refusal> {
    let machine = session.machine.is_empty() || uuid::Uuid::parse_str(&session.machine).is_ok();
    let length = session.name.chars().count();
    if !machine || !(1..=256).contains(&length) || session.name.chars().any(char::is_control) {
        return Err(Refusal::field(
            "invalid_field",
            field,
            "invalid",
            format!("`{field}` needs a machine UUID (or empty for this machine) and a session name of 1 to 256 characters"),
        ));
    }
    Ok(())
}

pub fn check_project(project: &str) -> Result<(), Refusal> {
    if project.len() > 128 || project.chars().any(char::is_control) {
        return Err(Refusal::field("invalid_field", "project", "invalid", "invalid project id"));
    }
    Ok(())
}

/// Receipts are keyed by the command and its request, so a request_id never
/// replays the result of a different command.
fn request_digest(command: &str, request: &Value) -> String {
    let keyed = json!({"command": command, "request": request});
    format!("{:x}", Sha256::digest(keyed.to_string().as_bytes()))
}

/// The stored result of an earlier request with this `request_id`; a reused ID
/// with another command or body is refused.
pub fn replay(tx: &Tx, command: &str, request: &Value) -> Result<Option<Value>> {
    let request_id = request.get("request_id").and_then(Value::as_str).unwrap_or_default();
    if request_id.is_empty() {
        return Ok(None);
    }
    if uuid::Uuid::parse_str(request_id).is_err() {
        return refuse("invalid_request", "request_id must be a UUID");
    }
    match tx.receipt(request_id)? {
        Some((stored, _)) if stored != request_digest(command, request) => {
            refuse("request_reused", "this request_id was already used for a different request")
        }
        Some((_, result)) => Ok(Some(result)),
        None => Ok(None),
    }
}

pub fn remember(tx: &mut Tx, command: &str, request: &Value, result: &Value) -> Result<()> {
    let request_id = request.get("request_id").and_then(Value::as_str).unwrap_or_default();
    if !request_id.is_empty() {
        tx.put_receipt(request_id, &request_digest(command, request), result)?;
    }
    Ok(())
}

/// Runs a write with receipt replay when the request carries a `request_id`.
/// `command` is the CLI command name.
pub fn write(
    ctx: &mut Ctx,
    command: &str,
    request: &Value,
    f: impl FnOnce(&mut Tx, &Actor) -> Result<Value>,
) -> Result<Value> {
    let actor = ctx.actor.clone();
    ctx.store.write(|tx| {
        if let Some(result) = replay(tx, command, request)? {
            return Ok(result);
        }
        let result = f(tx, &actor)?;
        remember(tx, command, request, &result)?;
        Ok(result)
    })
}

pub fn resolve(tx: &Tx, input: &str) -> Result<String> {
    Ok(refs::resolve(input, &tx.task_ids()?)?)
}

pub fn reference(tx: &Tx, uuid: &str) -> Result<String> {
    Ok(refs::refs(&tx.task_ids()?).remove(uuid).unwrap_or_default())
}

pub fn workflow(tx: &Tx, project: &str) -> Result<Workflow> {
    let shared = tx.load(&Filter::Shared(project.into()))?;
    let refs: Vec<&Event> = shared.iter().collect();
    Ok(project_workflow(&refs, project))
}

fn context(tx: &Tx, uuid: &str, extra: Option<&Event>) -> Result<Option<Task>> {
    let mut events = tx.load(&Filter::Task(uuid.into()))?;
    let project = match events.iter().find(|e| e.kind == "task_created").or(extra) {
        Some(created) => created.project.clone(),
        None => return Ok(None),
    };
    events.extend(tx.load(&Filter::Shared(project))?);
    if let Some(extra) = extra {
        events.push(extra.clone());
    }
    let refs: Vec<&Event> = events.iter().collect();
    Ok(fold(uuid, &refs))
}

pub fn load(tx: &Tx, uuid: &str) -> Result<Task> {
    match context(tx, uuid, None)? {
        Some(task) if !task.deleted => Ok(task),
        _ => refuse("unknown_task", "no such task"),
    }
}

fn read_only() -> Refusal {
    Refusal::new("read_only", "this task was written by a newer version of hgs; update hgs on this machine to change it")
}

/// Like `load`, for commands that write to the task: a task that holds events
/// from a newer version is shown but never changed.
pub fn load_writable(tx: &Tx, uuid: &str) -> Result<Task> {
    let task = load(tx, uuid)?;
    if task.read_only {
        return Err(read_only().into());
    }
    Ok(task)
}

/// Appends `draft` only if folding accepts it.
pub fn prove(tx: &mut Tx, draft: Draft) -> Result<Event> {
    let event = tx.preview(&draft)?;
    let task = context(tx, &draft.task, Some(&event))?;
    if let Some(refusal) = task.as_ref().and_then(|t| t.rejections.get(&event.id)) {
        return Err(refusal.clone().into());
    }
    tx.insert(&event)?;
    Ok(event)
}

#[allow(clippy::too_many_arguments)]
pub fn transition(
    tx: &mut Tx,
    actor: &Actor,
    task: &Task,
    action: Action,
    to: &str,
    fields: BTreeMap<String, String>,
    session: Option<SessionRef>,
    children: Vec<String>,
    request_id: &str,
) -> Result<Event> {
    let fields: BTreeMap<String, String> = fields.into_iter().map(|(k, v)| (k, normalize(&v))).collect();
    for (name, value) in &fields {
        check_text(name, value, MAX_FIELD, true)?;
    }
    let mut payload = json!({"action": action, "to": to, "fields": fields, "session": session, "children": children});
    // A task that changes stage joins the end of its new column.
    let target = if action == Action::Answer { task.resume_stage.as_str() } else { to };
    if target != task.stage {
        payload["rank"] = json!(checked_rank(next_rank(tx, &task.project, target)?)?);
    }
    prove(
        tx,
        Draft {
            actor: actor.clone(),
            task: task.uuid.clone(),
            project: task.project.clone(),
            kind: "transition".into(),
            base: task.head.clone(),
            request_id: request_id.into(),
            payload,
        },
    )
}

pub fn simple(tx: &mut Tx, actor: &Actor, task: &Task, kind: &str, payload: Value, request_id: &str) -> Result<Event> {
    prove(
        tx,
        Draft {
            actor: actor.clone(),
            task: task.uuid.clone(),
            project: task.project.clone(),
            kind: kind.into(),
            base: String::new(),
            request_id: request_id.into(),
            payload,
        },
    )
}

pub fn view(task: &Task, reference: &str, full: bool) -> Value {
    let mut out = json!({
        "uuid": task.uuid, "ref": reference, "project": task.project, "title": task.title,
        "stage": task.stage, "priority": task.priority, "rank": task.rank, "labels": task.labels,
        "pinned": task.pinned, "assignee": task.assignee, "reviewer": task.reviewer,
        "flags": task.flags, "parent": task.parent, "follows": task.follows,
        "created_at": task.created_at, "updated_at": task.updated_at,
        "read_only": task.read_only, "deleted": task.deleted, "journal_count": task.journal.len(),
    });
    if full {
        out["prompt"] = json!(task.prompt);
        out["attachments"] = json!(task.attachments);
        out["journal"] = json!(task.journal);
        // The event of the last transition: its journal entries are the current ones.
        out["head"] = json!(task.head);
        out["related"] = json!(task.related);
        out["resume_stage"] = json!(task.resume_stage);
        out["created_by"] = json!(task.created_by);
    }
    out
}

pub fn task_result(tx: &Tx, uuid: &str) -> Result<Value> {
    let task = load(tx, uuid)?;
    Ok(json!({"ok": true, "task": view(&task, &reference(tx, uuid)?, true)}))
}

fn default_title(prompt: &str) -> String {
    prompt
        .lines()
        .map(str::trim)
        .find(|line| !line.is_empty())
        .unwrap_or_default()
        .chars()
        .take(MAX_TITLE)
        .map(|c| if c.is_control() { ' ' } else { c })
        .collect()
}

fn check_title(title: &str) -> Result<(), Refusal> {
    if title.chars().count() > MAX_TITLE || title.chars().any(char::is_control) {
        return Err(Refusal::field("invalid_field", "title", "invalid", format!("a title has at most {MAX_TITLE} characters on one line")));
    }
    Ok(())
}

fn check_prompt(prompt: &str) -> Result<(), Refusal> {
    check_text("prompt", prompt, MAX_PROMPT, true)?;
    if prompt.trim().is_empty() {
        return Err(Refusal::field("missing_field", "prompt", "empty", "write the task's prompt"));
    }
    Ok(())
}

#[derive(Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct NewAttachment {
    pub name: String,
    pub mime: String,
    pub data_base64: String,
}

pub fn store_attachments(store: &Store, items: &[NewAttachment]) -> Result<Vec<Attachment>> {
    let bad = |message: String| -> anyhow::Error { Refusal::field("invalid_field", "attachments", "invalid", message).into() };
    if items.len() > MAX_ATTACHMENTS {
        return Err(bad(format!("at most {MAX_ATTACHMENTS} images per task")));
    }
    let mut total = 0;
    let mut decoded = Vec::new();
    for item in items {
        if item.name.is_empty() || item.name.len() > 255 || item.name.contains('/') || item.name.chars().any(char::is_control) {
            return Err(bad("invalid attachment name".into()));
        }
        if !item.mime.starts_with("image/") || item.mime.len() <= "image/".len() || item.mime.len() > 100 {
            return Err(bad("attachments must be images".into()));
        }
        let bytes = base64::engine::general_purpose::STANDARD
            .decode(item.data_base64.as_bytes())
            .map_err(|_| bad("attachment data is not base64".into()))?;
        if bytes.is_empty() {
            return Err(bad(format!("attachment {} is empty", item.name)));
        }
        total += bytes.len();
        if bytes.len() > MAX_ATTACHMENT || total > MAX_ATTACHMENTS_TOTAL {
            return Err(bad("images are limited to 10 MiB each and 20 MiB in total".into()));
        }
        decoded.push(bytes);
    }
    let mut out = Vec::new();
    for (item, bytes) in items.iter().zip(decoded) {
        let sha256 = blobs::put(store.dir(), &bytes)?;
        out.push(Attachment { name: item.name.clone(), mime: item.mime.clone(), bytes: bytes.len() as u64, sha256 });
    }
    Ok(out)
}

fn board_tasks(tx: &Tx, project: &str) -> Result<Vec<Task>> {
    let events = tx.load(&Filter::Project(project.into()))?;
    Ok(fold_many(&events).into_values().filter(|t| t.project == project).collect())
}

/// The rank at the end of a column. It never writes: when the column's ranks
/// are exhausted, it ties with the last task, and the tie orders by UUID.
pub fn next_rank(tx: &Tx, project: &str, stage: &str) -> Result<String> {
    let max = board_tasks(tx, project)?
        .iter()
        .filter(|t| !t.deleted && t.stage == stage)
        .map(|t| t.rank.clone())
        .max()
        .unwrap_or_default();
    let next = rank::after(&max);
    Ok(if rank::valid(&next) { next } else { max })
}

/// Ranks are computed by commands; an invalid one is a bug and is never written.
fn checked_rank(rank: String) -> Result<String> {
    anyhow::ensure!(rank::valid(&rank), "internal error: computed an invalid rank {rank:?}");
    Ok(rank)
}

#[allow(clippy::too_many_arguments)]
pub fn created_payload(
    title: &str,
    prompt: &str,
    stage: &str,
    priority: Option<u8>,
    labels: &[String],
    pinned: Option<SessionRef>,
    parent: &str,
    follows: BTreeSet<String>,
    related: BTreeSet<String>,
    attachments: Vec<Attachment>,
    fields: BTreeMap<String, String>,
    rank: String,
) -> Result<Value> {
    let prompt = normalize(prompt);
    let prompt = prompt.as_str();
    let fields: BTreeMap<String, String> = fields.into_iter().map(|(k, v)| (k, normalize(&v))).collect();
    check_prompt(prompt)?;
    let title = if title.trim().is_empty() { default_title(prompt) } else { title.trim().to_owned() };
    check_title(&title)?;
    let priority = priority.unwrap_or(2);
    if priority > 4 {
        return Err(Refusal::field("invalid_field", "priority", "invalid", "priority is 0 (highest) to 4").into());
    }
    if labels.len() > MAX_LABELS {
        return Err(Refusal::field("invalid_field", "labels", "invalid", format!("at most {MAX_LABELS} labels")).into());
    }
    for label in labels {
        check_label(label)?;
    }
    for (name, value) in &fields {
        check_text(name, value, MAX_FIELD, true)?;
    }
    let rank = checked_rank(rank)?;
    Ok(json!({
        "title": title, "prompt": prompt, "stage": stage, "priority": priority, "rank": rank,
        "labels": labels, "pinned": pinned, "parent": parent, "follows": follows, "related": related,
        "attachments": attachments, "fields": fields,
    }))
}

fn task_uuid(request_id: &str) -> String {
    uuid::Uuid::parse_str(request_id)
        .map(|u| u.hyphenated().to_string())
        .unwrap_or_else(|_| uuid::Uuid::new_v4().to_string())
}

/// A session as a person names it; an empty or missing machine means this one.
#[derive(Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct NamedSession {
    #[serde(default)]
    machine: String,
    name: String,
}

impl From<NamedSession> for SessionRef {
    fn from(session: NamedSession) -> Self {
        SessionRef { machine: session.machine, name: session.name }
    }
}

/// Validates a session reference and fills in this machine when none is named.
pub fn local_session(field: &str, session: Option<SessionRef>, machine: &str) -> Result<Option<SessionRef>, Refusal> {
    let Some(mut session) = session else {
        return Ok(None);
    };
    check_session(field, &session)?;
    session.machine = match uuid::Uuid::parse_str(&session.machine) {
        Ok(id) => id.hyphenated().to_string(),
        Err(_) => machine.into(),
    };
    Ok(Some(session))
}

pub fn rev(ctx: &mut Ctx) -> Result<Value> {
    Ok(json!({"ok": true, "cursor": ctx.store.cursor()?}))
}

pub fn status(ctx: &mut Ctx) -> Result<Value> {
    let vector = ctx.store.version_vector()?;
    let peers: Vec<Value> = ctx
        .store
        .peers()?
        .into_iter()
        .map(|peer| {
            let behind = peer.vector.as_ref().map(|theirs| {
                vector.iter().map(|(origin, seq)| seq.saturating_sub(theirs.get(origin).copied().unwrap_or(0))).sum::<u64>()
            });
            json!({"machine": peer.machine, "alias": peer.alias, "last_ok": peer.last_ok,
                   "last_error": peer.last_error, "updated": peer.updated, "behind": behind})
        })
        .collect();
    // Changes of this store's origin and collisions found in exchanges, newest
    // first: unlike a peer's last error, a clean exchange never clears them.
    let history = ctx.store.origin_history(20)?;
    Ok(json!({"ok": true, "machine": ctx.store.machine(), "origin": ctx.store.origin(), "cursor": ctx.store.cursor()?,
              "vector": vector, "peers": peers, "origin_history": history}))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct BoardRequest {
    #[serde(default)]
    project: String,
    #[serde(default)]
    since: Option<i64>,
}

pub fn board(ctx: &mut Ctx, request: Value) -> Result<Value> {
    let req: BoardRequest = parse(&request)?;
    let cursor = ctx.store.cursor()?;
    let touched = match req.since {
        Some(since) => Some(ctx.store.touched_since(since)?),
        None => None,
    };
    let events = ctx.store.load(&Filter::Project(req.project.clone()))?;
    let refs: Vec<&Event> = events.iter().collect();
    let wf = project_workflow(&refs, &req.project);
    let names = refs::refs(&ctx.store.task_ids()?);
    let shared_changed = touched.as_ref().is_some_and(|t| t.contains(""));
    let mut tasks: Vec<Task> = fold_many(&events)
        .into_values()
        .filter(|t| t.project == req.project)
        .filter(|t| shared_changed || touched.as_ref().map_or(true, |set| set.contains(&t.uuid)))
        .filter(|t| !t.deleted || touched.as_ref().is_some_and(|set| set.contains(&t.uuid)))
        .collect();
    let position = |stage: &str| wf.stages.iter().position(|s| s.name == stage).unwrap_or(usize::MAX);
    tasks.sort_by(|a, b| {
        position(&a.stage)
            .cmp(&position(&b.stage))
            .then(a.rank.cmp(&b.rank))
            .then(a.uuid.cmp(&b.uuid))
    });
    let machine = ctx.store.machine();
    let renames = super::sessions::renames_of(&events, machine);
    let views: Vec<Value> = tasks
        .iter()
        .map(|t| {
            let mut view = view(t, names.get(&t.uuid).map(String::as_str).unwrap_or_default(), false);
            with_current_pin(&mut view, t, &renames, machine);
            view["queue_of"] = if wf.role(&t.stage) == Some(Role::Ready) {
                json!(view["pinned_current"].as_str().map_or_else(
                    || t.pinned.clone(),
                    |name| t.pinned.as_ref().map(|pinned| SessionRef { machine: pinned.machine.clone(), name: name.to_owned() })
                ))
            } else {
                Value::Null
            };
            view["markers"] = json!(markers(t, &wf));
            view
        })
        .collect();
    Ok(json!({"ok": true, "project": req.project, "cursor": cursor, "workflow": wf, "tasks": views}))
}

/// What a card shows besides its stage: `working` while a session holds the
/// task in an active stage, the review verdict, and the flags a person acts on.
fn markers(task: &Task, wf: &Workflow) -> Vec<&'static str> {
    let mut out = Vec::new();
    if task.assignee.is_some() && wf.role(&task.stage) == Some(Role::Active) {
        out.push("working");
    }
    for (name, marker) in [("reviewed", "reviewed"), ("review-failed", "needs_work")] {
        if task.labels.contains(name) {
            out.push(marker);
        }
    }
    for (name, marker) in [("conflict", "conflict"), ("delivery_uncertain", "uncertain"), ("orphaned", "orphaned")] {
        if task.flags.contains(name) {
            out.push(marker);
        }
    }
    out
}

fn clip(text: &str, max: usize) -> String {
    text.chars().take(max).collect()
}

const MAX_DECISIONS: usize = 50;

/// The question the task is parked with: the `[needs-human]` entry written by
/// its last transition. Anything else (a newer-version transition, say) is not.
fn last_question(task: &Task) -> Option<&super::fold::JournalEntry> {
    task.journal.iter().rev().find(|j| j.marker == "[needs-human]").filter(|j| j.event == task.head)
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct NoFields {}

/// Tasks waiting for a person in every project, oldest question first. The log
/// is folded once. `signature` changes whenever the task does, so a watcher
/// notifies once per change.
pub fn decisions(ctx: &mut Ctx, request: Value) -> Result<Value> {
    let _: NoFields = parse(&request)?;
    let cursor = ctx.store.cursor()?;
    let events = ctx.store.load(&Filter::All)?;
    let names = refs::refs(&ctx.store.task_ids()?);
    let all: Vec<&Event> = events.iter().collect();
    let mut workflows: BTreeMap<String, Workflow> = BTreeMap::new();
    let mut waiting: Vec<Task> = Vec::new();
    for task in fold_many(&events).into_values() {
        if task.deleted {
            continue;
        }
        let wf = workflows.entry(task.project.clone()).or_insert_with(|| project_workflow(&all, &task.project));
        if wf.role(&task.stage) != Some(Role::NeedsHuman) {
            continue;
        }
        waiting.push(task);
    }
    // Oldest first by the position of the task's last transition in the totally
    // ordered log (event times only have a resolution of one second).
    let heads: BTreeSet<&str> = waiting.iter().map(|t| t.head.as_str()).collect();
    let position: BTreeMap<&str, (u64, &str, u64)> =
        events.iter().filter(|e| heads.contains(e.id.as_str())).map(|e| (e.id.as_str(), e.order())).collect();
    waiting.sort_by(|a, b| position.get(a.head.as_str()).cmp(&position.get(b.head.as_str())).then(a.uuid.cmp(&b.uuid)));
    let decisions: Vec<Value> = waiting
        .into_iter()
        .take(MAX_DECISIONS)
        .map(|task| {
            let question = last_question(&task).map(|j| clip(&j.text, 500)).unwrap_or_default();
            json!({
                "task": task.uuid, "ref": names.get(&task.uuid).map(String::as_str).unwrap_or_default(),
                "project": task.project, "title": clip(&task.title, 120), "stage": task.stage,
                "question": question, "assignee": task.assignee,
                "signature": format!("{:x}", Sha256::digest(format!("{}{}", task.uuid, task.head).as_bytes())),
            })
        })
        .collect();
    Ok(json!({"ok": true, "cursor": cursor, "decisions": decisions}))
}

/// Projects that hold tasks, with a count per stage role (every role is
/// present, zero when empty). Deleted tasks do not count.
pub fn projects(ctx: &mut Ctx, request: Value) -> Result<Value> {
    let _: NoFields = parse(&request)?;
    let cursor = ctx.store.cursor()?;
    let events = ctx.store.load(&Filter::All)?;
    let all: Vec<&Event> = events.iter().collect();
    let mut workflows: BTreeMap<String, Workflow> = BTreeMap::new();
    let mut counts: BTreeMap<String, (u64, BTreeMap<Role, u64>)> = BTreeMap::new();
    for task in fold_many(&events).into_values().filter(|t| !t.deleted) {
        let wf = workflows.entry(task.project.clone()).or_insert_with(|| project_workflow(&all, &task.project));
        let entry = counts.entry(task.project.clone()).or_default();
        entry.0 += 1;
        if let Some(role) = wf.role(&task.stage) {
            *entry.1.entry(role).or_default() += 1;
        }
    }
    let list: Vec<Value> = counts
        .into_iter()
        .map(|(project, (total, by_role))| {
            let roles: BTreeMap<String, u64> = [Role::Backlog, Role::Ready, Role::Active, Role::Review, Role::NeedsHuman, Role::Done, Role::Parked]
                .into_iter()
                .map(|role| (json!(role).as_str().unwrap_or_default().to_owned(), by_role.get(&role).copied().unwrap_or(0)))
                .collect();
            json!({"project": project, "total": total, "roles": roles})
        })
        .collect();
    Ok(json!({"ok": true, "cursor": cursor, "projects": list}))
}

/// Adds `pinned_current` to a task's view when the task is pinned to a session
/// of this machine that has another name now: a pin written on another machine
/// can miss a rename there. Other machines show the pin as written.
fn with_current_pin(view: &mut Value, task: &Task, renames: &[(String, String)], machine: &str) {
    if let Some(pinned) = task.pinned.as_ref().filter(|pinned| pinned.machine == machine) {
        let name = super::sessions::current_name(renames, &pinned.name, &crate::state::session_tracked);
        if name != pinned.name {
            view["pinned_current"] = json!(name);
        }
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct TaskRequest {
    #[serde(default)]
    request_id: String,
    task: String,
}

pub fn show(ctx: &mut Ctx, request: Value) -> Result<Value> {
    let req: TaskRequest = parse(&request)?;
    let machine = ctx.store.machine().to_owned();
    ctx.store.read(|tx| {
        let uuid = resolve(tx, &req.task)?;
        let task = load(tx, &uuid)?;
        let mut view = view(&task, &reference(tx, &uuid)?, true);
        let renames = super::sessions::renames_of(&tx.load(&Filter::Shared(String::new()))?, &machine);
        with_current_pin(&mut view, &task, &renames, &machine);
        Ok(json!({"ok": true, "task": view}))
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct CreateRequest {
    #[serde(default)]
    request_id: String,
    #[serde(default)]
    project: String,
    #[serde(default)]
    title: String,
    prompt: String,
    #[serde(default)]
    stage: String,
    #[serde(default)]
    priority: Option<u8>,
    #[serde(default)]
    labels: Vec<String>,
    #[serde(default)]
    pinned: Option<NamedSession>,
    #[serde(default)]
    attachments: Vec<NewAttachment>,
    #[serde(default)]
    parent: String,
    #[serde(default)]
    follows: Vec<String>,
}

pub fn create(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let mut req: CreateRequest = parse(&request)?;
    req.prompt = normalize(&req.prompt);
    check_project(&req.project)?;
    for label in &req.labels {
        check_reserved(label)?;
    }
    let pinned = local_session("pinned", req.pinned.clone().map(Into::into), ctx.store.machine())?;
    if pinned.is_none() && req.project.is_empty() {
        return Err(Refusal::field("missing_field", "project", "missing", "choose a project or pin the task to a session").into());
    }
    check_prompt(&req.prompt)?;
    let attachments = store_attachments(ctx.store, &req.attachments)?;
    write(ctx, "create", &request, |tx, actor| {
        let wf = workflow(tx, &req.project)?;
        let stage = if req.stage.is_empty() {
            wf.first(Role::Backlog).map(|s| s.name.clone()).unwrap_or_default()
        } else {
            req.stage.clone()
        };
        let follows = req.follows.iter().map(|r| resolve(tx, r)).collect::<Result<BTreeSet<_>>>()?;
        let parent = if req.parent.is_empty() { String::new() } else { resolve(tx, &req.parent)? };
        let rank = next_rank(tx, &req.project, &stage)?;
        let payload = created_payload(&req.title, &req.prompt, &stage, req.priority, &req.labels, pinned.clone(), &parent, follows, BTreeSet::new(), attachments.clone(), BTreeMap::new(), rank)?;
        let uuid = task_uuid(&req.request_id);
        if tx.task_ids()?.contains(&uuid) {
            return refuse("request_reused", "a task with this request_id already exists");
        }
        prove(tx, Draft { actor: actor.clone(), task: uuid.clone(), project: req.project.clone(), kind: "task_created".into(), base: String::new(), request_id: req.request_id.clone(), payload })?;
        task_result(tx, &uuid)
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct EditRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    #[serde(default)]
    title: Option<String>,
    #[serde(default)]
    prompt: Option<String>,
    #[serde(default)]
    priority: Option<u8>,
    /// The task's whole new list of images: new ones and references.
    #[serde(default)]
    attachments: Option<Vec<Value>>,
}

/// An image the task already has, kept as stored: named by its digest alone.
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct KeptAttachment {
    sha256: String,
}

/// One place in an edited list of images.
enum EditImage {
    /// A reference, resolved against the task inside the write.
    Keep(String),
    /// The index of a new image among those stored for the request.
    New(usize),
}

fn is_digest(text: &str) -> bool {
    text.len() == 64 && text.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f'))
}

/// Splits an edited list into its order and its new images. Each item is
/// either a new image `{name, mime, data_base64}` or a reference `{sha256}`;
/// both shapes are strict, so a mix of the two is refused.
fn edit_images(items: &[Value]) -> Result<(Vec<EditImage>, Vec<NewAttachment>), Refusal> {
    let bad = |message: String| Refusal::field("invalid_field", "attachments", "invalid", message);
    let mut order = Vec::new();
    let mut new = Vec::new();
    for item in items {
        if item.get("sha256").is_some() {
            let kept: KeptAttachment = serde_json::from_value(item.clone())
                .map_err(|_| bad("an image the task keeps is named by its sha256 alone".into()))?;
            if !is_digest(&kept.sha256) {
                return Err(bad(format!("{:?} is not an image digest", kept.sha256)));
            }
            order.push(EditImage::Keep(kept.sha256));
        } else {
            let image: NewAttachment = serde_json::from_value(item.clone()).map_err(|e| bad(format!("invalid image: {e}")))?;
            order.push(EditImage::New(new.len()));
            new.push(image);
        }
    }
    Ok((order, new))
}

pub fn edit(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let mut req: EditRequest = parse(&request)?;
    req.prompt = req.prompt.map(|p| normalize(&p));
    // New images are stored before the write; references need no bytes.
    let images = match &req.attachments {
        Some(items) => {
            let (order, new) = edit_images(items)?;
            Some((order, store_attachments(ctx.store, &new)?))
        }
        None => None,
    };
    write(ctx, "edit", &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let mut changes: Vec<(&str, Value)> = Vec::new();
        if let Some(title) = &req.title {
            let title = title.trim();
            if title.is_empty() {
                return Err(Refusal::field("missing_field", "title", "empty", "a title cannot be empty").into());
            }
            check_title(title)?;
            changes.push(("title", json!(title)));
        }
        if let Some(prompt) = &req.prompt {
            check_prompt(prompt)?;
            changes.push(("prompt", json!(prompt)));
        }
        if let Some(priority) = req.priority {
            if priority > 4 {
                return Err(Refusal::field("invalid_field", "priority", "invalid", "priority is 0 (highest) to 4").into());
            }
            changes.push(("priority", json!(priority)));
        }
        if let Some((order, stored)) = &images {
            let task = load_writable(tx, &uuid)?;
            let bad = |message: String| -> anyhow::Error { Refusal::field("invalid_field", "attachments", "invalid", message).into() };
            let mut list: Vec<Attachment> = Vec::new();
            for image in order {
                list.push(match image {
                    EditImage::Keep(sha) => task
                        .attachments
                        .iter()
                        .find(|a| &a.sha256 == sha)
                        .cloned()
                        .ok_or_else(|| bad(format!("this task has no image {sha}")))?,
                    EditImage::New(index) => stored[*index].clone(),
                });
            }
            // The limits apply to the list the task ends with.
            if list.len() > MAX_ATTACHMENTS {
                return Err(bad(format!("at most {MAX_ATTACHMENTS} images per task")));
            }
            let total = list.iter().map(|a| a.bytes).fold(0u64, u64::saturating_add);
            if list.iter().any(|a| a.bytes > MAX_ATTACHMENT as u64) || total > MAX_ATTACHMENTS_TOTAL as u64 {
                return Err(bad("images are limited to 10 MiB each and 20 MiB in total".into()));
            }
            changes.push(("attachments", json!(list)));
        }
        for (field, value) in changes {
            let task = load_writable(tx, &uuid)?;
            simple(tx, actor, &task, "field_set", json!({"field": field, "value": value}), &req.request_id)?;
        }
        task_result(tx, &uuid)
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct MoveRequest {
    #[allow(dead_code)] // Read from the raw request by the receipt layer; declared for deny_unknown_fields.
    #[serde(default)]
    request_id: String,
    task: String,
    to: String,
}

fn move_to(
    ctx: &mut Ctx,
    command: &str,
    request: &Value,
    task_ref: &str,
    target: impl Fn(&Workflow, &Task) -> Result<String>,
) -> Result<Value> {
    human_only(&ctx.actor)?;
    let request_id = request.get("request_id").and_then(Value::as_str).unwrap_or_default().to_owned();
    write(ctx, command, request, |tx, actor| {
        let uuid = resolve(tx, task_ref)?;
        let task = load_writable(tx, &uuid)?;
        let wf = workflow(tx, &task.project)?;
        let to = target(&wf, &task)?;
        let from_role = wf.role(&task.stage).unwrap_or(Role::Backlog);
        let to_role = wf.role(&to).ok_or_else(|| Refusal::new("unknown_stage", format!("unknown stage {to}")))?;
        let action = match (from_role, to_role) {
            (Role::Active, Role::Ready) => Action::Unclaim,
            (Role::Review, Role::Done) => Action::Done,
            (Role::Done, Role::Ready) => Action::Reopen,
            (_, Role::Active) if from_role != Role::Active => return refuse("use_send", "send the task to a session to start it"),
            (Role::NeedsHuman, _) if to_role != Role::Parked => return refuse("use_answer", "answer the question to resume the task"),
            _ => Action::Move,
        };
        transition(tx, actor, &task, action, &to, BTreeMap::new(), None, Vec::new(), &request_id)?;
        task_result(tx, &uuid)
    })
}

pub fn move_task(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: MoveRequest = parse(&request)?;
    let to = req.to.clone();
    move_to(ctx, "move", &request, &req.task, move |_, _| Ok(to.clone()))
}

pub fn done(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: TaskRequest = parse(&request)?;
    move_to(ctx, "done", &request, &req.task, |wf, _| wf.first(Role::Done).map(|s| s.name.clone()).ok_or_else(|| Refusal::new("unknown_stage", "the workflow has no Done stage").into()))
}

pub fn reopen(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: TaskRequest = parse(&request)?;
    move_to(ctx, "reopen", &request, &req.task, |wf, task| {
        if wf.role(&task.stage) != Some(Role::Done) {
            return Err(Refusal::new("transition_not_allowed", "only a finished task can be reopened").into());
        }
        wf.first(Role::Ready).map(|s| s.name.clone()).ok_or_else(|| Refusal::new("unknown_stage", "the workflow has no Ready stage").into())
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ReorderRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    #[serde(default)]
    before: Option<String>,
}

pub fn reorder(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: ReorderRequest = parse(&request)?;
    write(ctx, "reorder", &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load_writable(tx, &uuid)?;
        // The other tasks of the column, in display order.
        let mut column: Vec<Task> = board_tasks(tx, &task.project)?
            .into_iter()
            .filter(|t| !t.deleted && t.stage == task.stage && t.uuid != uuid)
            .collect();
        column.sort_by(|a, b| a.rank.cmp(&b.rank).then(a.uuid.cmp(&b.uuid)));
        let mut target = match &req.before {
            Some(before) => Some(load(tx, &resolve(tx, before)?)?.uuid),
            None => None,
        };
        if target.as_deref() == Some(uuid.as_str()) {
            target = None;
        }
        let position = match &target {
            Some(before) => Some(column.iter().position(|t| &t.uuid == before).ok_or_else(|| {
                Refusal::field("invalid_field", "before", "invalid", "`before` must be a task in the same stage")
            })?),
            None => None,
        };
        let mut rank = place(&column, position);
        if rank.is_none() {
            // Ties or exhausted ranks: renumber the column evenly, keeping its order.
            if column.iter().any(|t| t.read_only) {
                return Err(read_only().into());
            }
            let fresh_ranks = rank::spread(column.len());
            for (other, fresh) in column.iter_mut().zip(fresh_ranks) {
                simple(tx, actor, other, "rank_set", json!({"rank": checked_rank(fresh.clone())?}), "")?;
                other.rank = fresh;
            }
            rank = place(&column, position);
        }
        let rank = checked_rank(rank.unwrap_or_default())?;
        simple(tx, actor, &task, "rank_set", json!({"rank": rank}), &req.request_id)?;
        task_result(tx, &uuid)
    })
}

/// The rank that puts a task before `column[position]`, or at the end of the
/// column. None when ties or exhausted ranks need the column renumbered first.
fn place(column: &[Task], position: Option<usize>) -> Option<String> {
    let Some(i) = position else {
        let next = rank::after(column.last().map_or("", |t| t.rank.as_str()));
        return rank::valid(&next).then_some(next);
    };
    if column.windows(2).any(|pair| pair[0].rank >= pair[1].rank) {
        return None;
    }
    let previous = if i == 0 { "" } else { column[i - 1].rank.as_str() };
    let next = column[i].rank.as_str();
    let rank = rank::between(previous, Some(next));
    (rank::valid(&rank) && previous < rank.as_str() && rank.as_str() < next).then_some(rank)
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct PinRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    #[serde(default)]
    session: Option<NamedSession>,
}

pub fn pin(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: PinRequest = parse(&request)?;
    let session = local_session("session", req.session.clone().map(Into::into), ctx.store.machine())?;
    write(ctx, "pin", &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load_writable(tx, &uuid)?;
        simple(tx, actor, &task, "pin_set", json!({"session": session}), &req.request_id)?;
        task_result(tx, &uuid)
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct UnpinRequest {
    #[serde(default)]
    request_id: String,
    task: String,
}

pub fn unpin(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: UnpinRequest = parse(&request)?;
    write(ctx, "unpin", &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load_writable(tx, &uuid)?;
        simple(tx, actor, &task, "pin_set", json!({"session": null}), &req.request_id)?;
        task_result(tx, &uuid)
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct LabelRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    label: String,
    present: bool,
}

pub fn label(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: LabelRequest = parse(&request)?;
    check_label(&req.label)?;
    check_reserved(&req.label)?;
    write(ctx, "label", &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load_writable(tx, &uuid)?;
        if req.present && !task.labels.contains(&req.label) && task.labels.len() >= MAX_LABELS {
            return Err(Refusal::field("invalid_field", "labels", "invalid", format!("a task has at most {MAX_LABELS} labels")).into());
        }
        simple(tx, actor, &task, "label_set", json!({"label": req.label, "present": req.present}), &req.request_id)?;
        task_result(tx, &uuid)
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct LinkRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    relation: String,
    target: String,
    present: bool,
}

pub fn link(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: LinkRequest = parse(&request)?;
    if !matches!(req.relation.as_str(), "parent" | "follows" | "related") {
        return Err(Refusal::field("invalid_field", "relation", "invalid", "relation is parent, follows or related").into());
    }
    write(ctx, "link", &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let target = resolve(tx, &req.target)?;
        if target == uuid {
            return Err(Refusal::field("invalid_field", "target", "invalid", "a task cannot refer to itself").into());
        }
        let task = load_writable(tx, &uuid)?;
        simple(tx, actor, &task, "relation_set", json!({"relation": req.relation, "target": target, "present": req.present}), &req.request_id)?;
        task_result(tx, &uuid)
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct CommentRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    text: String,
}

pub fn comment(ctx: &mut Ctx, request: Value) -> Result<Value> {
    let mut req: CommentRequest = parse(&request)?;
    req.text = normalize(&req.text);
    check_text("text", &req.text, MAX_FIELD, true)?;
    if req.text.trim().is_empty() {
        return Err(Refusal::field("missing_field", "text", "empty", "write the comment").into());
    }
    write(ctx, "comment", &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load_writable(tx, &uuid)?;
        if actor.kind == ActorKind::Agent {
            let me = actor.session_ref();
            if me.is_none() || (me != task.assignee && me != task.reviewer) {
                return refuse("not_assigned", "this task is not assigned to your session");
            }
        }
        simple(tx, actor, &task, "journal", json!({"marker": "[comment]", "text": req.text}), &req.request_id)?;
        task_result(tx, &uuid)
    })
}

pub fn delete(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: TaskRequest = parse(&request)?;
    write(ctx, "delete", &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load_writable(tx, &uuid)?;
        simple(tx, actor, &task, "task_deleted", json!({}), &req.request_id)?;
        Ok(json!({"ok": true, "deleted": uuid}))
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ProjectRequest {
    #[serde(default)]
    project: String,
}

pub fn workflow_get(ctx: &mut Ctx, request: Value) -> Result<Value> {
    let req: ProjectRequest = parse(&request)?;
    let wf = ctx.store.read(|tx| workflow(tx, &req.project))?;
    Ok(json!({"ok": true, "project": req.project, "workflow": wf}))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct WorkflowSetRequest {
    #[serde(default)]
    request_id: String,
    #[serde(default)]
    project: String,
    workflow: Value,
    #[serde(default)]
    moves: BTreeMap<String, String>,
}

pub fn workflow_set(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: WorkflowSetRequest = parse(&request)?;
    check_project(&req.project)?;
    let wf: Workflow = serde_json::from_value(req.workflow.clone())
        .map_err(|e| Refusal::new("invalid_workflow", format!("invalid workflow: {e}")))?;
    wf.validate().map_err(|e| Refusal::new("invalid_workflow", format!("{e:#}")))?;
    write(ctx, "workflow set", &request, |tx, actor| {
        for task in board_tasks(tx, &req.project)?.iter().filter(|t| !t.deleted) {
            if wf.stage(&task.stage).is_none() && !req.moves.get(&task.stage).is_some_and(|to| wf.stage(to).is_some()) {
                return refuse("moves_required", format!("stage {} still holds tasks; say where to move them in `moves`", task.stage));
            }
        }
        tx.append(&Draft {
            actor: actor.clone(),
            task: String::new(),
            project: req.project.clone(),
            kind: "workflow_set".into(),
            base: String::new(),
            request_id: req.request_id.clone(),
            payload: json!({"workflow": wf, "moves": req.moves}),
        })?;
        Ok(json!({"ok": true, "project": req.project, "workflow": wf}))
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    const MACHINE: &str = "11111111-1111-4111-8111-111111111111";

    fn store() -> (tempfile::TempDir, Store) {
        let dir = tempfile::tempdir().unwrap();
        let store = Store::open(dir.path(), MACHINE).unwrap();
        (dir, store)
    }
    fn human(store: &mut Store) -> Ctx<'_> {
        Ctx { store, actor: Actor::human(MACHINE), cwd: None }
    }
    fn new_task(store: &mut Store, stage: &str) -> String {
        let result = create(&mut human(store), json!({"project": "p", "prompt": "Fix the drafts\nDetails", "stage": stage})).unwrap();
        result["task"]["uuid"].as_str().unwrap().to_owned()
    }
    fn refusal(error: anyhow::Error) -> Refusal {
        error.downcast::<Refusal>().unwrap()
    }
    fn column(store: &mut Store, stage: &str) -> Vec<String> {
        let board_now = board(&mut human(store), json!({"project": "p"})).unwrap();
        board_now["tasks"]
            .as_array()
            .unwrap()
            .iter()
            .filter(|t| t["stage"] == stage)
            .map(|t| t["uuid"].as_str().unwrap().to_owned())
            .collect()
    }

    #[test]
    fn create_defaults_title_stage_priority_and_rank() {
        let (_dir, mut store) = store();
        let first = create(&mut human(&mut store), json!({"project": "p", "prompt": "\n  Fix the drafts  \nmore"})).unwrap();
        assert_eq!(first["task"]["title"], "Fix the drafts");
        assert_eq!(first["task"]["stage"], "Backlog");
        assert_eq!(first["task"]["priority"], 2);
        assert!(first["task"]["ref"].as_str().unwrap().starts_with("om-"));
        let second = create(&mut human(&mut store), json!({"project": "p", "prompt": "Second"})).unwrap();
        assert!(second["task"]["rank"].as_str().unwrap() > first["task"]["rank"].as_str().unwrap());
    }

    #[test]
    fn status_shows_the_origin_history_newest_first() {
        let (_dir, mut store) = store();
        let peer = "22222222-2222-4222-8222-222222222222";
        let old = store.origin().to_owned();
        let fresh = store.rotate_origin_from(&old, "restored", peer).unwrap().unwrap();
        for _ in 0..25 {
            store.note_collision(&uuid::Uuid::new_v4().to_string(), peer).unwrap();
        }
        let shown = status(&mut human(&mut store)).unwrap();
        let history = shown["origin_history"].as_array().unwrap();
        assert_eq!(history.len(), 20, "at most 20");
        assert_eq!((history[0]["kind"].as_str(), history[0]["peer"].as_str()), (Some("collision"), Some(peer)));
        assert!(history[0].get("replaced_by").is_none() && history[0]["at"].as_u64().unwrap() > 0);
        let (_dir, mut other) = self::store();
        let first = other.origin().to_owned();
        other.rotate_origin_from(&first, "restored", peer).unwrap();
        let only = status(&mut human(&mut other)).unwrap()["origin_history"].clone();
        assert_eq!((only[0]["kind"].as_str(), only[0]["replaced_by"].as_str()), (Some("restored"), Some(other.origin())));
        assert_ne!(fresh, old);
    }

    #[test]
    fn create_requires_a_project_or_a_pin() {
        let (_dir, mut store) = store();
        let error = refusal(create(&mut human(&mut store), json!({"prompt": "x"})).unwrap_err());
        assert_eq!(error.field, "project");
        let pinned = create(&mut human(&mut store), json!({"prompt": "x", "stage": "Queue", "pinned": {"machine": "", "name": "claude/zerus/w1"}})).unwrap();
        assert_eq!(pinned["task"]["pinned"]["machine"], MACHINE);
    }

    #[test]
    fn limits_count_bytes_for_prompts_and_characters_for_titles() {
        let (_dir, mut store) = store();
        let exact = "ж".repeat(MAX_PROMPT / 2);
        assert_eq!(exact.len(), MAX_PROMPT);
        assert!(create(&mut human(&mut store), json!({"project": "p", "prompt": exact})).is_ok());
        let over = format!("{exact}x");
        assert_eq!(refusal(create(&mut human(&mut store), json!({"project": "p", "prompt": over})).unwrap_err()).field, "prompt");
        let title = "я".repeat(MAX_TITLE);
        assert!(create(&mut human(&mut store), json!({"project": "p", "prompt": "x", "title": title})).is_ok());
        let long = "я".repeat(MAX_TITLE + 1);
        assert_eq!(refusal(create(&mut human(&mut store), json!({"project": "p", "prompt": "x", "title": long})).unwrap_err()).field, "title");
        let control = create(&mut human(&mut store), json!({"project": "p", "prompt": "a\u{0007}b"})).unwrap_err();
        assert_eq!(refusal(control).state, "invalid");
    }

    #[test]
    fn receipts_replay_and_refuse_reuse() {
        let (_dir, mut store) = store();
        let id = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
        let request = json!({"request_id": id, "project": "p", "prompt": "x"});
        let first = create(&mut human(&mut store), request.clone()).unwrap();
        assert_eq!(first["task"]["uuid"], id, "a request UUID becomes the task UUID");
        edit(&mut human(&mut store), json!({"task": id, "title": "Changed"})).unwrap();
        assert_eq!(create(&mut human(&mut store), request).unwrap(), first);
        let reused = create(&mut human(&mut store), json!({"request_id": id, "project": "p", "prompt": "y"})).unwrap_err();
        assert_eq!(refusal(reused).code, "request_reused");
        assert_eq!(store.task_ids().unwrap().len(), 1);
    }

    #[test]
    fn a_request_id_never_replays_another_command() {
        let (_dir, mut store) = store();
        let task = new_task(&mut store, "Queue");
        let id = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
        pin(&mut human(&mut store), json!({"request_id": id, "task": task})).unwrap();
        let reused = delete(&mut human(&mut store), json!({"request_id": id, "task": task})).unwrap_err();
        assert_eq!(refusal(reused).code, "request_reused");
        assert_eq!(show(&mut human(&mut store), json!({"task": task})).unwrap()["task"]["uuid"], task);
    }

    #[test]
    fn tasks_with_events_from_a_newer_version_refuse_writes() {
        let (_dir, mut store) = store();
        let task = new_task(&mut store, "Queue");
        store
            .write(|tx| {
                let mut event = tx.preview(&Draft {
                    actor: Actor::human(MACHINE),
                    task: task.clone(),
                    project: "p".into(),
                    kind: "field_set".into(),
                    base: String::new(),
                    request_id: String::new(),
                    payload: json!({"field": "title", "value": "x"}),
                })?;
                event.v = EVENT_VERSION + 1;
                tx.insert(&event)
            })
            .unwrap();
        let commented = comment(&mut human(&mut store), json!({"task": task, "text": "note"})).unwrap_err();
        assert_eq!(refusal(commented).code, "read_only");
        let moved = move_task(&mut human(&mut store), json!({"task": task, "to": "Backlog"})).unwrap_err();
        assert_eq!(refusal(moved).code, "read_only");
        let shown = show(&mut human(&mut store), json!({"task": task})).unwrap();
        assert_eq!((shown["task"]["read_only"].as_bool(), shown["task"]["stage"].as_str()), (Some(true), Some("Queue")));
    }

    #[test]
    fn moves_choose_the_action_and_respect_gates() {
        let (_dir, mut store) = store();
        let task = new_task(&mut store, "Backlog");
        let moved = move_task(&mut human(&mut store), json!({"task": task, "to": "Queue"})).unwrap();
        assert_eq!(moved["task"]["stage"], "Queue");
        let start = refusal(move_task(&mut human(&mut store), json!({"task": task, "to": "Design"})).unwrap_err());
        assert_eq!(start.code, "use_send");
        let skip = refusal(done(&mut human(&mut store), json!({"task": task})).unwrap_err());
        assert_eq!(skip.code, "transition_not_allowed");
        let iced = move_task(&mut human(&mut store), json!({"task": task, "to": "Icebox"})).unwrap();
        assert_eq!(iced["task"]["stage"], "Icebox");
    }

    #[test]
    fn agents_cannot_run_human_commands() {
        let (_dir, mut store) = store();
        let task = new_task(&mut store, "Queue");
        let mut ctx = Ctx { store: &mut store, actor: Actor::agent(MACHINE, "w1"), cwd: None };
        for result in [
            create(&mut ctx, json!({"project": "p", "prompt": "x"})),
            edit(&mut ctx, json!({"task": task, "title": "x"})),
            move_task(&mut ctx, json!({"task": task, "to": "Backlog"})),
            done(&mut ctx, json!({"task": task})),
            delete(&mut ctx, json!({"task": task})),
        ] {
            assert_eq!(refusal(result.unwrap_err()).code, "human_only");
        }
    }

    #[test]
    fn board_lists_tasks_in_stage_and_rank_order_and_since_a_cursor() {
        let (_dir, mut store) = store();
        let a = new_task(&mut store, "Queue");
        let b = new_task(&mut store, "Queue");
        reorder(&mut human(&mut store), json!({"task": b, "before": a})).unwrap();
        let board_all = board(&mut human(&mut store), json!({"project": "p"})).unwrap();
        let order: Vec<&str> = board_all["tasks"].as_array().unwrap().iter().map(|t| t["uuid"].as_str().unwrap()).collect();
        assert_eq!(order, vec![b.as_str(), a.as_str()]);
        let cursor = board_all["cursor"].as_i64().unwrap();
        label(&mut human(&mut store), json!({"task": a, "label": "ui", "present": true})).unwrap();
        let delta = board(&mut human(&mut store), json!({"project": "p", "since": cursor})).unwrap();
        assert_eq!(delta["tasks"].as_array().unwrap().len(), 1);
        assert_eq!(delta["tasks"][0]["labels"], json!(["ui"]));
    }

    fn claim(store: &mut Store, task: &str, name: &str) {
        store
            .write(|tx| {
                let current = load(tx, task)?;
                transition(tx, &Actor::human(MACHINE), &current, Action::Claim, "Design", BTreeMap::new(), Some(SessionRef { machine: MACHINE.into(), name: name.into() }), vec![], "")?;
                Ok(())
            })
            .unwrap();
    }
    fn ask(store: &mut Store, task: &str, name: &str, question: &str) {
        let ctx = &mut Ctx { store, actor: Actor::agent(MACHINE, name), cwd: None };
        super::super::agent::call_human(ctx, json!({"task": task, "question": question})).unwrap();
    }

    #[test]
    fn decisions_list_your_call_tasks_oldest_first_with_signatures() {
        let (_dir, mut store) = store();
        let a = new_task(&mut store, "Queue");
        let b = new_task(&mut store, "Queue");
        let quiet = new_task(&mut store, "Queue");
        let gone = new_task(&mut store, "Queue");
        for (task, name) in [(&b, "w2"), (&a, "w1"), (&gone, "w3")] {
            claim(&mut store, task, name);
        }
        ask(&mut store, &b, "w2", "Which API?");
        ask(&mut store, &a, "w1", &"q".repeat(900));
        ask(&mut store, &gone, "w3", "Dropped?");
        delete(&mut human(&mut store), json!({"task": gone})).unwrap();
        let result = decisions(&mut human(&mut store), json!({})).unwrap();
        assert_eq!(result["ok"], true);
        assert_eq!(result["cursor"], store.cursor().unwrap());
        let list = result["decisions"].as_array().unwrap();
        let order: Vec<&str> = list.iter().map(|d| d["task"].as_str().unwrap()).collect();
        assert_eq!(order, vec![b.as_str(), a.as_str()]);
        assert!(!order.contains(&quiet.as_str()));
        let first = &list[0];
        assert_eq!(first["project"], "p");
        assert_eq!(first["stage"], "Your Call");
        assert_eq!(first["question"], "Which API?");
        assert_eq!(first["assignee"], json!({"machine": MACHINE, "name": "w2"}));
        assert_eq!(first["title"], "Fix the drafts");
        assert!(first["ref"].as_str().unwrap().len() >= 4);
        assert_eq!(list[1]["question"].as_str().unwrap().chars().count(), 500);
        let signature = first["signature"].as_str().unwrap();
        assert_eq!(signature.len(), 64);
        // The signature names the task's last transition: stable across polls
        // and comments, different per task.
        let again = decisions(&mut human(&mut store), json!({})).unwrap();
        assert_eq!(again["decisions"][0]["signature"], signature);
        comment(&mut human(&mut store), json!({"task": b, "text": "More context"})).unwrap();
        let changed = decisions(&mut human(&mut store), json!({})).unwrap();
        assert_eq!(changed["decisions"][0]["signature"], signature);
        assert_ne!(list[1]["signature"], signature);
    }

    #[test]
    fn show_names_the_transition_the_task_is_in() {
        let (_dir, mut store) = store();
        let task = new_task(&mut store, "Queue");
        claim(&mut store, &task, "w1");
        ask(&mut store, &task, "w1", "Which API?");
        // A comment is newer but no transition: the question stays the current one.
        comment(&mut human(&mut store), json!({"task": task, "text": "More context"})).unwrap();
        let shown = show(&mut human(&mut store), json!({"task": task})).unwrap();
        let journal = shown["task"]["journal"].as_array().unwrap();
        let question = journal.iter().rev().find(|j| j["marker"] == "[needs-human]").unwrap();
        assert_eq!(shown["task"]["head"], question["event"]);
        assert_ne!(journal.last().unwrap()["event"], question["event"]);
        // Boards carry no journal and so no head.
        let listed = board(&mut human(&mut store), json!({"project": "p"})).unwrap();
        assert!(listed["tasks"][0].get("head").is_none(), "{listed}");
    }

    #[test]
    fn decisions_stop_at_fifty() {
        let (_dir, mut store) = store();
        for i in 0..52 {
            let task = new_task(&mut store, "Queue");
            claim(&mut store, &task, &format!("w{i}"));
            ask(&mut store, &task, &format!("w{i}"), "Which?");
        }
        let result = decisions(&mut human(&mut store), json!({})).unwrap();
        assert_eq!(result["decisions"].as_array().unwrap().len(), 50);
    }

    #[test]
    fn board_carries_the_workflow_queue_and_markers() {
        let (_dir, mut store) = store();
        let queued = new_task(&mut store, "Queue");
        pin(&mut human(&mut store), json!({"task": queued, "session": {"name": "w9"}})).unwrap();
        let working = new_task(&mut store, "Queue");
        claim(&mut store, &working, "w1");
        let tagged = new_task(&mut store, "Queue");
        label(&mut human(&mut store), json!({"task": tagged, "label": "ui", "present": true})).unwrap();
        let result = board(&mut human(&mut store), json!({"project": "p"})).unwrap();
        assert_eq!(result["workflow"]["stages"][0]["role"], "backlog");
        let find = |uuid: &str| result["tasks"].as_array().unwrap().iter().find(|t| t["uuid"] == uuid).unwrap().clone();
        assert_eq!(find(&queued)["queue_of"], json!({"machine": MACHINE, "name": "w9"}));
        assert_eq!(find(&queued)["markers"], json!([]));
        assert_eq!(find(&working)["queue_of"], Value::Null);
        assert_eq!(find(&working)["markers"], json!(["working"]));
        assert_eq!(find(&tagged)["markers"], json!([]));
    }

    #[test]
    fn markers_name_verdicts_and_flags() {
        let (_dir, mut store) = store();
        let id = new_task(&mut store, "Queue");
        let mut task = store.read(|tx| load(tx, &id)).unwrap();
        let wf = Workflow::default_v1();
        assert!(markers(&task, &wf).is_empty());
        task.labels.insert("reviewed".into());
        task.labels.insert("review-failed".into());
        for flag in ["conflict", "delivery_uncertain", "orphaned"] {
            task.flags.insert(flag.into());
        }
        assert_eq!(markers(&task, &wf), vec!["reviewed", "needs_work", "conflict", "uncertain", "orphaned"]);
    }

    #[test]
    fn decisions_and_projects_refuse_unknown_fields_and_the_queue_shows_only_ready_tasks() {
        let (_dir, mut store) = store();
        assert_eq!(refusal(decisions(&mut human(&mut store), json!({"x": 1})).unwrap_err()).code, "invalid_request");
        assert_eq!(refusal(projects(&mut human(&mut store), json!({"x": 1})).unwrap_err()).code, "invalid_request");
        let backlog = new_task(&mut store, "Backlog");
        pin(&mut human(&mut store), json!({"task": backlog, "session": {"name": "w9"}})).unwrap();
        let result = board(&mut human(&mut store), json!({"project": "p"})).unwrap();
        assert_eq!(result["tasks"][0]["pinned"]["name"], "w9");
        assert_eq!(result["tasks"][0]["queue_of"], Value::Null);
    }

    #[test]
    fn projects_count_tasks_per_role_and_skip_empty_projects() {
        let (_dir, mut store) = store();
        let a = new_task(&mut store, "Queue");
        new_task(&mut store, "Queue");
        new_task(&mut store, "Backlog");
        claim(&mut store, &a, "w1");
        ask(&mut store, &a, "w1", "Which?");
        let other = create(&mut human(&mut store), json!({"project": "q", "prompt": "Other"})).unwrap()["task"]["uuid"].as_str().unwrap().to_owned();
        delete(&mut human(&mut store), json!({"task": other})).unwrap();
        let result = projects(&mut human(&mut store), json!({})).unwrap();
        assert_eq!(result["cursor"], store.cursor().unwrap());
        let list = result["projects"].as_array().unwrap();
        assert_eq!(list.len(), 1);
        assert_eq!(list[0]["project"], "p");
        assert_eq!(list[0]["total"], 3);
        assert_eq!(list[0]["roles"]["ready"], 1);
        assert_eq!(list[0]["roles"]["backlog"], 1);
        assert_eq!(list[0]["roles"]["needs_human"], 1);
        assert_eq!(list[0]["roles"]["done"], 0);
    }

    #[test]
    fn repeated_reorders_renumber_the_column_and_keep_its_order() {
        let (_dir, mut store) = store();
        let a = new_task(&mut store, "Queue");
        let b = new_task(&mut store, "Queue");
        let c = new_task(&mut store, "Queue");
        for i in 0..80 {
            let moved = if i % 2 == 0 { &b } else { &c };
            reorder(&mut human(&mut store), json!({"task": moved, "before": a})).unwrap();
        }
        let board_all = board(&mut human(&mut store), json!({"project": "p"})).unwrap();
        let order: Vec<&str> = board_all["tasks"].as_array().unwrap().iter().map(|t| t["uuid"].as_str().unwrap()).collect();
        // The last move put c (i = 79) directly before a, and b came before c.
        assert_eq!(order, vec![b.as_str(), c.as_str(), a.as_str()]);
        for t in board_all["tasks"].as_array().unwrap() {
            assert!(rank::valid(t["rank"].as_str().unwrap()));
        }
    }

    #[test]
    fn moving_into_a_column_puts_the_task_at_its_end() {
        let (_dir, mut store) = store();
        let a = new_task(&mut store, "Queue");
        let b = new_task(&mut store, "Queue");
        let first = new_task(&mut store, "Backlog");
        let moved = new_task(&mut store, "Backlog");
        // Its Backlog rank now sorts below every Queue rank.
        reorder(&mut human(&mut store), json!({"task": moved, "before": first})).unwrap();
        move_task(&mut human(&mut store), json!({"task": moved, "to": "Queue"})).unwrap();
        assert_eq!(column(&mut store, "Queue"), vec![a, b, moved]);
    }

    #[test]
    fn reorder_honours_the_drop_target_when_ranks_tie() {
        let (_dir, mut store) = store();
        let mut tied: Vec<String> = (0..3).map(|_| new_task(&mut store, "Queue")).collect();
        let moved = new_task(&mut store, "Queue");
        store
            .write(|tx| {
                for uuid in &tied {
                    let task = load(tx, uuid)?;
                    simple(tx, &Actor::human(MACHINE), &task, "rank_set", json!({"rank": "V"}), "")?;
                }
                Ok(())
            })
            .unwrap();
        tied.sort(); // Tied ranks show in UUID order.
        assert_eq!(column(&mut store, "Queue"), vec![tied[0].clone(), tied[1].clone(), tied[2].clone(), moved.clone()]);
        reorder(&mut human(&mut store), json!({"task": moved, "before": tied[1]})).unwrap();
        assert_eq!(column(&mut store, "Queue"), vec![tied[0].clone(), moved.clone(), tied[1].clone(), tied[2].clone()]);
        let board_now = board(&mut human(&mut store), json!({"project": "p"})).unwrap();
        let ranks: Vec<&str> = board_now["tasks"].as_array().unwrap().iter().map(|t| t["rank"].as_str().unwrap()).collect();
        assert!(ranks.iter().all(|r| rank::valid(r)));
        assert!(ranks.windows(2).all(|pair| pair[0] < pair[1]), "{ranks:?}");
    }

    #[test]
    fn many_creations_in_one_column_keep_valid_ranks_and_their_order() {
        let (_dir, mut store) = store();
        let created: Vec<String> = (0..400).map(|_| new_task(&mut store, "Queue")).collect();
        let board_now = board(&mut human(&mut store), json!({"project": "p"})).unwrap();
        assert!(board_now["tasks"].as_array().unwrap().iter().all(|t| rank::valid(t["rank"].as_str().unwrap())));
        assert_eq!(column(&mut store, "Queue"), created);
    }

    #[test]
    fn agent_comments_on_closed_tasks_are_refused_by_the_fold() {
        let (_dir, mut store) = store();
        let uuid = new_task(&mut store, "Queue");
        let agent = Actor::agent(MACHINE, "w1");
        let session = agent.session_ref();
        store
            .write(|tx| {
                let task = load(tx, &uuid)?;
                transition(tx, &Actor::human(MACHINE), &task, Action::Claim, "Design", BTreeMap::new(), session.clone(), vec![], "")?;
                let task = load(tx, &uuid)?;
                transition(tx, &Actor::human(MACHINE), &task, Action::Move, "Icebox", BTreeMap::new(), None, vec![], "")?;
                Ok(())
            })
            .unwrap();
        let mut ctx = Ctx { store: &mut store, actor: agent, cwd: None };
        let error = comment(&mut ctx, json!({"task": uuid, "text": "late note"})).unwrap_err();
        assert_eq!(refusal(error).code, "task_closed");
    }

    #[test]
    fn crlf_text_is_normalized_and_accepted() {
        let (_dir, mut store) = store();
        let made = create(&mut human(&mut store), json!({"project": "p", "prompt": "Title\r\nbody\rend"})).unwrap();
        assert_eq!(made["task"]["prompt"], "Title\nbody\nend");
        let uuid = made["task"]["uuid"].as_str().unwrap().to_owned();
        comment(&mut human(&mut store), json!({"task": uuid, "text": "a\r\nb"})).unwrap();
        let tab = create(&mut human(&mut store), json!({"project": "p", "prompt": "a\tb\nc"})).unwrap();
        assert_eq!(tab["task"]["title"], "a b");
    }

    #[test]
    fn reorder_reopen_and_attachments_validate_up_front() {
        let (_dir, mut store) = store();
        let a = new_task(&mut store, "Queue");
        let b = new_task(&mut store, "Backlog");
        let other = refusal(reorder(&mut human(&mut store), json!({"task": a, "before": b})).unwrap_err());
        assert_eq!(other.field, "before");
        reorder(&mut human(&mut store), json!({"task": a, "before": a})).unwrap();
        let reopened = refusal(reopen(&mut human(&mut store), json!({"task": a})).unwrap_err());
        assert_eq!(reopened.code, "transition_not_allowed");
        let png = base64::engine::general_purpose::STANDARD.encode(b"\x89PNG");
        let bad = json!({"project": "p", "prompt": "x", "attachments": [
            {"name": "a.png", "mime": "image/png", "data_base64": png},
            {"name": "b.png", "mime": "image/", "data_base64": png}]});
        assert_eq!(refusal(create(&mut human(&mut store), bad).unwrap_err()).field, "attachments");
        let sha = format!("{:x}", Sha256::digest(b"\x89PNG"));
        assert!(!blobs::path(store.dir(), &sha).unwrap().exists());
    }

    #[test]
    fn show_comment_pin_link_and_delete() {
        let (_dir, mut store) = store();
        let a = new_task(&mut store, "Queue");
        let b = new_task(&mut store, "Queue");
        comment(&mut human(&mut store), json!({"task": a, "text": "note"})).unwrap();
        pin(&mut human(&mut store), json!({"task": a, "session": {"machine": "", "name": "w1"}})).unwrap();
        link(&mut human(&mut store), json!({"task": a, "relation": "follows", "target": b, "present": true})).unwrap();
        let shown = show(&mut human(&mut store), json!({"task": a})).unwrap();
        assert_eq!(shown["task"]["pinned"]["name"], "w1");
        assert_eq!(shown["task"]["follows"], json!([b]));
        assert!(shown["task"]["journal"].as_array().unwrap().iter().any(|j| j["marker"] == "[comment]"));
        assert_eq!(refusal(link(&mut human(&mut store), json!({"task": a, "relation": "follows", "target": a, "present": true})).unwrap_err()).code, "invalid_field");
        assert_eq!(refusal(label(&mut human(&mut store), json!({"task": a, "label": "reviewed", "present": true})).unwrap_err()).code, "reserved_label");
        delete(&mut human(&mut store), json!({"task": a})).unwrap();
        assert_eq!(refusal(show(&mut human(&mut store), json!({"task": a})).unwrap_err()).code, "unknown_task");
    }

    #[test]
    fn pins_name_this_machine_without_a_machine_and_unpin_removes_them() {
        let (_dir, mut store) = store();
        let task = new_task(&mut store, "Queue");
        let pinned = pin(&mut human(&mut store), json!({"task": task, "session": {"name": "w1"}})).unwrap();
        assert_eq!(pinned["task"]["pinned"], json!({"machine": MACHINE, "name": "w1"}));
        let unpinned = unpin(&mut human(&mut store), json!({"task": task})).unwrap();
        assert_eq!(unpinned["task"]["pinned"], Value::Null);
        let created = create(&mut human(&mut store), json!({"prompt": "x", "pinned": {"name": "w2"}})).unwrap();
        assert_eq!(created["task"]["pinned"], json!({"machine": MACHINE, "name": "w2"}));
        let unknown = pin(&mut human(&mut store), json!({"task": task, "session": {"name": "w1", "extra": 1}})).unwrap_err();
        assert_eq!(refusal(unknown).code, "invalid_request");
    }

    #[test]
    fn labels_and_session_references_are_checked() {
        let (_dir, mut store) = store();
        let reserved = create(&mut human(&mut store), json!({"project": "p", "prompt": "x", "labels": ["epic"]})).unwrap_err();
        assert_eq!(refusal(reserved).code, "reserved_label");
        let labels: Vec<String> = (0..32).map(|i| format!("l{i}")).collect();
        let full = create(&mut human(&mut store), json!({"project": "p", "prompt": "x", "labels": labels})).unwrap();
        let uuid = full["task"]["uuid"].as_str().unwrap().to_owned();
        let more = label(&mut human(&mut store), json!({"task": uuid, "label": "extra", "present": true})).unwrap_err();
        assert_eq!(refusal(more).field, "labels");
        label(&mut human(&mut store), json!({"task": uuid, "label": "l0", "present": true})).unwrap();
        label(&mut human(&mut store), json!({"task": uuid, "label": "l0", "present": false})).unwrap();
        label(&mut human(&mut store), json!({"task": uuid, "label": "extra", "present": true})).unwrap();
        for session in [json!({"machine": "not-a-uuid", "name": "w1"}), json!({"machine": "", "name": ""}), json!({"machine": "", "name": "a\u{7}b"}), json!({"machine": "", "name": "n".repeat(257)})] {
            let pinned = create(&mut human(&mut store), json!({"project": "p", "prompt": "x", "pinned": session})).unwrap_err();
            assert_eq!(refusal(pinned).field, "pinned");
            let pinned = pin(&mut human(&mut store), json!({"task": uuid, "session": session})).unwrap_err();
            assert_eq!(refusal(pinned).field, "session");
        }
        let upper = pin(&mut human(&mut store), json!({"task": uuid, "session": {"machine": MACHINE.to_uppercase(), "name": "w1"}})).unwrap();
        assert_eq!(upper["task"]["pinned"]["machine"], MACHINE);
    }

    #[test]
    fn attachments_are_stored_by_digest_within_limits() {
        let (_dir, mut store) = store();
        let png = base64::engine::general_purpose::STANDARD.encode(b"\x89PNG");
        let result = create(&mut human(&mut store), json!({"project": "p", "prompt": "x", "attachments": [{"name": "a.png", "mime": "image/png", "data_base64": png}]})).unwrap();
        let sha = result["task"]["attachments"][0]["sha256"].as_str().unwrap().to_owned();
        assert!(blobs::path(store.dir(), &sha).unwrap().exists());
        let text = create(&mut human(&mut store), json!({"project": "p", "prompt": "x", "attachments": [{"name": "a.txt", "mime": "text/plain", "data_base64": png}]})).unwrap_err();
        assert_eq!(refusal(text).field, "attachments");
        let empty = create(&mut human(&mut store), json!({"project": "p", "prompt": "x", "attachments": [{"name": "a.png", "mime": "image/png", "data_base64": ""}]})).unwrap_err();
        assert_eq!(refusal(empty).field, "attachments");
    }

    fn image(bytes: &[u8], name: &str) -> Value {
        json!({"name": name, "mime": "image/png", "data_base64": base64::engine::general_purpose::STANDARD.encode(bytes)})
    }

    #[test]
    fn edit_keeps_images_by_digest_and_adds_new_ones() {
        let (_dir, mut store) = store();
        let created = create(&mut human(&mut store), json!({"project": "p", "prompt": "x", "attachments": [image(b"\x89PNG a", "a.png")]})).unwrap();
        let uuid = created["task"]["uuid"].as_str().unwrap().to_owned();
        let a = created["task"]["attachments"][0].clone();
        // Keep a as stored, add b.
        let both = edit(&mut human(&mut store), json!({"task": uuid, "attachments": [{"sha256": a["sha256"]}, image(b"\x89PNG b", "b.png")]})).unwrap();
        let list = both["task"]["attachments"].as_array().unwrap().clone();
        assert_eq!(list.len(), 2);
        assert_eq!(list[0], a);
        assert_eq!(list[1]["name"], "b.png");
        assert!(blobs::path(store.dir(), list[1]["sha256"].as_str().unwrap()).unwrap().exists());
        // Keep only b, then none.
        let kept = edit(&mut human(&mut store), json!({"task": uuid, "attachments": [{"sha256": list[1]["sha256"]}]})).unwrap();
        assert_eq!(kept["task"]["attachments"], json!([list[1]]));
        let none = edit(&mut human(&mut store), json!({"task": uuid, "attachments": []})).unwrap();
        assert_eq!(none["task"]["attachments"], json!([]));
    }

    #[test]
    fn edit_refuses_unknown_and_malformed_image_references() {
        let (_dir, mut store) = store();
        let created = create(&mut human(&mut store), json!({"project": "p", "prompt": "x", "attachments": [image(b"\x89PNG a", "a.png")]})).unwrap();
        let uuid = created["task"]["uuid"].as_str().unwrap().to_owned();
        let a = created["task"]["attachments"][0].clone();
        let sha = a["sha256"].as_str().unwrap().to_owned();
        let second = new_task(&mut store, "Queue");
        let other = edit(&mut human(&mut store), json!({"task": second, "attachments": [image(b"\x89PNG c", "c.png")]})).unwrap();
        let elsewhere = other["task"]["attachments"][0]["sha256"].clone();
        for wrong in [json!({"sha256": "0".repeat(64)}), json!({"sha256": elsewhere}), json!({"sha256": "xyz"}), json!({"sha256": sha.to_uppercase()}),
                      json!({"sha256": sha, "name": "a.png"}), json!({"name": "a.png"}),
                      json!({"name": "b.png", "mime": "image/png", "data_base64": "AA==", "note": "x"})] {
            let refused = refusal(edit(&mut human(&mut store), json!({"task": uuid, "attachments": [{"sha256": sha}, wrong.clone()]})).unwrap_err());
            assert_eq!((refused.code.as_str(), refused.field.as_str()), ("invalid_field", "attachments"), "{wrong}");
        }
        let shown = show(&mut human(&mut store), json!({"task": uuid})).unwrap();
        assert_eq!(shown["task"]["attachments"], json!([a]));
    }

    #[test]
    fn edit_limits_apply_to_the_final_list() {
        let (_dir, mut store) = store();
        let eight: Vec<Value> = (0..8).map(|n| image(format!("PNG {n}").as_bytes(), &format!("{n}.png"))).collect();
        let created = create(&mut human(&mut store), json!({"project": "p", "prompt": "x", "attachments": eight})).unwrap();
        let uuid = created["task"]["uuid"].as_str().unwrap().to_owned();
        let mut list: Vec<Value> = created["task"]["attachments"].as_array().unwrap().iter().map(|a| json!({"sha256": a["sha256"]})).collect();
        list.push(image(b"\x89PNG ninth", "9.png"));
        let refused = refusal(edit(&mut human(&mut store), json!({"task": uuid, "attachments": list})).unwrap_err());
        assert_eq!(refused.field, "attachments");
        // Two images at the size limit fill the total: one more byte is refused,
        // although it is the only new image.
        let full = create(&mut human(&mut store), json!({"project": "p", "prompt": "x",
            "attachments": [image(&vec![1; MAX_ATTACHMENT], "1.png"), image(&vec![2; MAX_ATTACHMENT], "2.png")]})).unwrap();
        let uuid = full["task"]["uuid"].as_str().unwrap().to_owned();
        let kept: Vec<Value> = full["task"]["attachments"].as_array().unwrap().iter().map(|a| json!({"sha256": a["sha256"]})).collect();
        let over = refusal(edit(&mut human(&mut store), json!({"task": uuid, "attachments": [kept[0].clone(), kept[1].clone(), image(b"x", "x.png")]})).unwrap_err());
        assert_eq!(over.field, "attachments");
        let fits = edit(&mut human(&mut store), json!({"task": uuid, "attachments": [kept[0].clone(), image(b"x", "x.png")]})).unwrap();
        assert_eq!(fits["task"]["attachments"].as_array().unwrap().len(), 2);
    }

    #[test]
    fn workflow_set_requires_moves_for_occupied_stages() {
        let (_dir, mut store) = store();
        new_task(&mut store, "Queue");
        let mut wf = Workflow::default_v1();
        wf.stages[1].name = "Ready".into();
        for rule in &mut wf.rules {
            for f in &mut rule.from { if f == "Queue" { *f = "Ready".into(); } }
            if rule.to == "Queue" { rule.to = "Ready".into(); }
        }
        let refused = workflow_set(&mut human(&mut store), json!({"project": "p", "workflow": wf})).unwrap_err();
        assert_eq!(refusal(refused).code, "moves_required");
        workflow_set(&mut human(&mut store), json!({"project": "p", "workflow": wf, "moves": {"Queue": "Ready"}})).unwrap();
        let board_now = board(&mut human(&mut store), json!({"project": "p"})).unwrap();
        assert_eq!(board_now["tasks"][0]["stage"], "Ready");
        assert_eq!(workflow_get(&mut human(&mut store), json!({"project": "p"})).unwrap()["workflow"]["stages"][1]["name"], "Ready");
        let invalid = workflow_set(&mut human(&mut store), json!({"project": "p", "workflow": {"version": 2}})).unwrap_err();
        assert_eq!(refusal(invalid).code, "invalid_workflow");
    }
}
