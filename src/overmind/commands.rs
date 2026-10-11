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
const MAX_ATTACHMENTS: usize = 8;
const MAX_ATTACHMENT: usize = 10 * 1024 * 1024;
const MAX_ATTACHMENTS_TOTAL: usize = 20 * 1024 * 1024;
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

/// Runs a write with receipt replay when the request carries a `request_id`.
/// `command` is the CLI command name.
pub fn write(
    ctx: &mut Ctx,
    command: &str,
    request: &Value,
    f: impl FnOnce(&mut Tx, &Actor) -> Result<Value>,
) -> Result<Value> {
    let request_id = request.get("request_id").and_then(Value::as_str).unwrap_or_default().to_owned();
    if !request_id.is_empty() && uuid::Uuid::parse_str(&request_id).is_err() {
        return refuse("invalid_request", "request_id must be a UUID");
    }
    let digest = request_digest(command, request);
    let actor = ctx.actor.clone();
    ctx.store.write(|tx| {
        if !request_id.is_empty() {
            if let Some((stored, result)) = tx.receipt(&request_id)? {
                if stored != digest {
                    return refuse("request_reused", "this request_id was already used for a different request");
                }
                return Ok(result);
            }
        }
        let result = f(tx, &actor)?;
        if !request_id.is_empty() {
            tx.put_receipt(&request_id, &digest, &result)?;
        }
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

fn simple(tx: &mut Tx, actor: &Actor, task: &Task, kind: &str, payload: Value, request_id: &str) -> Result<Event> {
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
        out["related"] = json!(task.related);
        out["resume_stage"] = json!(task.resume_stage);
        out["created_by"] = json!(task.created_by);
    }
    out
}

fn task_result(tx: &Tx, uuid: &str) -> Result<Value> {
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
    if labels.len() > 32 {
        return Err(Refusal::field("invalid_field", "labels", "invalid", "at most 32 labels").into());
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

fn local_session(session: Option<SessionRef>, machine: &str) -> Option<SessionRef> {
    session.map(|mut s| {
        if s.machine.is_empty() {
            s.machine = machine.into();
        }
        s
    })
}

pub fn rev(ctx: &mut Ctx) -> Result<Value> {
    Ok(json!({"ok": true, "cursor": ctx.store.cursor()?}))
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
    let views: Vec<Value> = tasks
        .iter()
        .map(|t| view(t, names.get(&t.uuid).map(String::as_str).unwrap_or_default(), false))
        .collect();
    Ok(json!({"ok": true, "project": req.project, "cursor": cursor, "workflow": wf, "tasks": views}))
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
    ctx.store.read(|tx| {
        let uuid = resolve(tx, &req.task)?;
        task_result(tx, &uuid)
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
    pinned: Option<SessionRef>,
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
    let pinned = local_session(req.pinned.clone(), ctx.store.machine());
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
    #[serde(default)]
    attachments: Option<Vec<NewAttachment>>,
}

pub fn edit(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let mut req: EditRequest = parse(&request)?;
    req.prompt = req.prompt.map(|p| normalize(&p));
    let attachments = match &req.attachments {
        Some(items) => Some(store_attachments(ctx.store, items)?),
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
        if let Some(attachments) = &attachments {
            changes.push(("attachments", json!(attachments)));
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
    session: Option<SessionRef>,
}

pub fn pin(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: PinRequest = parse(&request)?;
    let session = local_session(req.session.clone(), ctx.store.machine());
    write(ctx, "pin", &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load_writable(tx, &uuid)?;
        simple(tx, actor, &task, "pin_set", json!({"session": session}), &req.request_id)?;
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
    if RESERVED_LABELS.contains(&req.label.as_str()) {
        return refuse("reserved_label", format!("`{}` is set by workflow actions", req.label));
    }
    write(ctx, "label", &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load_writable(tx, &uuid)?;
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
    fn attachments_are_stored_by_digest_within_limits() {
        let (_dir, mut store) = store();
        let png = base64::engine::general_purpose::STANDARD.encode(b"\x89PNG");
        let result = create(&mut human(&mut store), json!({"project": "p", "prompt": "x", "attachments": [{"name": "a.png", "mime": "image/png", "data_base64": png}]})).unwrap();
        let sha = result["task"]["attachments"][0]["sha256"].as_str().unwrap().to_owned();
        assert!(blobs::path(store.dir(), &sha).unwrap().exists());
        let text = create(&mut human(&mut store), json!({"project": "p", "prompt": "x", "attachments": [{"name": "a.txt", "mime": "text/plain", "data_base64": png}]})).unwrap_err();
        assert_eq!(refusal(text).field, "attachments");
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
