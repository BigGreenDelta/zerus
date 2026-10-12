//! Sending tasks into sessions on this machine (spec Part 5). The gates and the
//! session are checked first, then the claim is written, the text is typed like
//! `hgs send`, and the outcome is recorded. Overmind never types a text twice:
//! the input request ID is the Overmind request ID.
use super::blobs;
use super::commands::{
    self, human_only, load, load_writable, local_session, parse, reference, refuse, resolve, simple, transition, view,
    workflow, Ctx, NamedSession,
};
use super::fold::Task;
use super::model::*;
use super::store::{Filter, Store, Tx};
use super::templates;
use super::workflow::{Action, Role, Workflow};
use crate::state::DeliveryTarget;
use anyhow::Result;
use base64::Engine;
use serde::Deserialize;
use serde_json::{json, Value};
use std::collections::{BTreeMap, BTreeSet};
use std::path::Path;

pub enum Outcome {
    Sent,
    NotSent(String),
    Uncertain(String),
}

impl Outcome {
    fn name(&self) -> &'static str {
        match self {
            Outcome::Sent => "sent",
            Outcome::NotSent(_) => "not_sent",
            Outcome::Uncertain(_) => "uncertain",
        }
    }
    fn reason(&self) -> &str {
        match self {
            Outcome::Sent => "",
            Outcome::NotSent(reason) | Outcome::Uncertain(reason) => reason,
        }
    }
}

/// Refusal codes a person confirmed for this request.
pub type Confirmed = BTreeSet<String>;

pub fn confirmable(code: &str, confirmed: &Confirmed, message: String) -> Result<()> {
    if confirmed.contains(code) {
        return Ok(());
    }
    Err(Refusal::field(code, "confirm", "missing", message).into())
}

/// The tracked session on this machine that `session` names.
pub fn local_target(store: &Store, session: &SessionRef) -> Result<DeliveryTarget> {
    if session.machine != store.machine() {
        return refuse("remote_session", "this session is on another machine");
    }
    crate::state::delivery_target(&session.name)
        .map_err(|e| Refusal::field("session_unavailable", "session", "invalid", e))?
        .ok_or_else(|| {
            let message = format!("there is no session {} on {}", session.name, store.machine());
            Refusal::field("unknown_session", "session", "invalid", message).into()
        })
}

/// Refuses, writing nothing, when the session cannot take input now.
pub fn check_session(session: &SessionRef, target: &DeliveryTarget, confirmed: &Confirmed) -> Result<()> {
    if target.busy {
        confirmable("agent_busy", confirmed, format!("{} is working; the text will wait in its queue", session.name))?;
    }
    crate::state::delivery_preflight(&session.name, target)
        .map_err(|e| Refusal::field("session_unavailable", "session", "invalid", e).into())
}

/// The task's images as `hgs send` attachments, referenced as `[Image #n]`.
pub fn files(dir: &Path, task: &Task) -> Result<Vec<Value>> {
    let mut out = Vec::new();
    for (index, attachment) in task.attachments.iter().enumerate() {
        let bytes = blobs::path(dir, &attachment.sha256)
            .ok()
            .and_then(|path| std::fs::read(path).ok())
            .ok_or_else(|| {
                Refusal::field("attachment_unavailable", "attachments", "missing", format!("the image {} is not on this machine", attachment.name))
            })?;
        out.push(json!({
            "name": attachment.name, "mime": attachment.mime,
            "data_base64": base64::engine::general_purpose::STANDARD.encode(bytes),
            "reference": format!("[Image #{}]", index + 1),
        }));
    }
    Ok(out)
}

pub fn deliver(session: &SessionRef, target: &DeliveryTarget, input_id: &str, text: &str, files: Vec<Value>) -> Outcome {
    let request = json!({
        "request_id": input_id, "text": text, "expected_run_id": target.run_id,
        "expected_conversation_id": target.conversation_id, "attachments": files,
    });
    match crate::state::deliver_input(&session.name, &request) {
        Ok(receipt) if receipt["status"] == "submitted" => Outcome::Sent,
        Ok(_) => Outcome::Uncertain("the session did not confirm the input".into()),
        // Without a receipt nothing was typed; with one, the text may have been.
        Err(error) if crate::state::input_receipt_exists(input_id) => Outcome::Uncertain(error),
        Err(error) => Outcome::NotSent(error),
    }
}

/// The template `name` for `task`, as typed into `session`: its `{tools}` name
/// the MCP server only when Zerus attached it to the session's current run.
pub fn render(tx: &Tx, task: &Task, name: &str, answer: &str, session: &SessionRef) -> Result<String> {
    let template = templates::effective(tx, &task.project, name)?;
    let reference = reference(tx, &task.uuid)?;
    let tools = templates::tools(crate::state::overmind_mcp(&session.name));
    Ok(templates::render(&template, &templates::Vars { task, reference: &reference, answer, tools })?)
}

pub fn journal(tx: &mut Tx, actor: &Actor, task: &Task, marker: &str, text: &str, transition: &str) -> Result<()> {
    let mut payload = json!({"marker": marker, "text": text});
    if !transition.is_empty() {
        payload["transition"] = json!(transition);
    }
    simple(tx, actor, task, "journal", payload, "")?;
    Ok(())
}

pub fn flag(tx: &mut Tx, actor: &Actor, task: &Task, name: &str, value: bool) -> Result<()> {
    simple(tx, actor, task, "flag_set", json!({"flag": name, "value": value}), "")?;
    Ok(())
}

/// Where a person's `action` takes a task from `stage`, if the workflow allows it.
pub fn rule_target(wf: &Workflow, action: Action, stage: &str) -> Option<String> {
    wf.rules
        .iter()
        .find(|r| r.action == action && r.from.iter().any(|f| f == stage) && r.actors.contains(&ActorKind::Human))
        .map(|r| r.to.clone())
}

fn reason(text: &str) -> BTreeMap<String, String> {
    BTreeMap::from([("reason".to_owned(), text.to_owned())])
}

/// Tasks the session holds in active or needs-human stages, in every project.
fn active_tasks(tx: &Tx, session: &SessionRef) -> Result<usize> {
    Ok(super::sessions::holding(tx, session)?.len())
}

/// The stage a send claims the task into, after the gates of Part 3. It runs on
/// the session's host, which resolves a pin that missed one of its renames.
fn send_gates(tx: &Tx, machine: &str, wf: &Workflow, task: &Task, session: &SessionRef, confirmed: &Confirmed) -> Result<String> {
    if task.labels.contains("epic") {
        return refuse("epic", "an epic is a container and is never sent to a session");
    }
    let to = rule_target(wf, Action::Claim, &task.stage)
        .ok_or_else(|| Refusal::new("transition_not_allowed", format!("a task in {} cannot be sent", task.stage)))?;
    if let Some(pinned) = task.pinned.as_ref() {
        let pinned = super::sessions::current_pin(tx, pinned, machine)?;
        if pinned != *session {
            confirmable("pinned_elsewhere", confirmed, format!("this task is reserved for {}", pinned.name))?;
        }
    }
    let mut open = Vec::new();
    for uuid in &task.follows {
        if let Ok(before) = load(tx, uuid) {
            if !matches!(workflow(tx, &before.project)?.role(&before.stage), Some(Role::Review | Role::Done)) {
                open.push(reference(tx, uuid)?);
            }
        }
    }
    if !open.is_empty() {
        confirmable("predecessors_open", confirmed, format!("the tasks this one follows are not in review yet: {}", open.join(", ")))?;
    }
    let active = active_tasks(tx, session)?;
    if active >= wf.session_wip as usize {
        confirmable("wip_exceeded", confirmed, format!("{} already holds {active} active task(s)", session.name))?;
    }
    Ok(to)
}

/// The claim this request wrote whose outcome is not recorded yet: a send that
/// stopped between claiming and recording.
fn pending_claim(tx: &Tx, task: &Task, request_id: &str) -> Result<Option<Event>> {
    pending_transition(tx, task, request_id, "claim")
}

/// The head transition of `action` that this request wrote and no `[sent]` or
/// `[sent?]` entry references yet.
fn pending_transition(tx: &Tx, task: &Task, request_id: &str, action: &str) -> Result<Option<Event>> {
    if request_id.is_empty() {
        return Ok(None);
    }
    let events = tx.load(&Filter::Task(task.uuid.clone()))?;
    let Some(claim) = events.iter().find(|e| {
        e.id == task.head && e.kind == "transition" && e.request_id == request_id && e.payload["action"] == action
    }) else {
        return Ok(None);
    };
    Ok((!outcome_recorded(&events, claim)).then(|| claim.clone()))
}

fn outcome_recorded(events: &[Event], claim: &Event) -> bool {
    events.iter().any(|e| e.kind == "journal" && e.payload["transition"] == claim.id.as_str())
}

/// The claim is still the task's head and has no recorded outcome.
fn unsettled(tx: &Tx, task: &Task, claim: &Event) -> Result<bool> {
    let events = tx.load(&Filter::Task(task.uuid.clone()))?;
    Ok(task.head == claim.id && !outcome_recorded(&events, claim))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct SendRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    session: NamedSession,
    #[serde(default)]
    confirm: Confirmed,
}

/// Runs a sending command on the host of a session on another machine, before
/// anything is written here. On a host (`local_only`) it refuses instead: a
/// forwarded command is never forwarded again. The host's result is also this
/// request's receipt here, so a retry replays it without reaching the host and
/// the same `request_id` with another request is refused here.
fn route(ctx: &mut Ctx, command: &str, request: &Value, task: &str, session: &SessionRef, local_only: bool) -> Result<Value> {
    if local_only {
        let message = format!("the session is not on {}; no further forwarding", ctx.store.machine());
        return Err(Refusal::field("remote_session", "session", "invalid", message).into());
    }
    // The host may know more tasks, so a short reference could be ambiguous there.
    let uuid = ctx.store.read(|tx| resolve(tx, task))?;
    let mut forwarded = request.clone();
    forwarded["task"] = json!(uuid);
    let result = super::remote::forward(ctx.store, command, &forwarded, &uuid, &session.machine)?;
    record_forwarded(ctx.store, command, request, result)
}

/// Keeps the host's result as this request's receipt, but only for a request
/// that carries a `request_id`. The host has acted already, so a failure to
/// write the receipt is a warning and the host's result is still returned.
fn record_forwarded(store: &mut Store, command: &str, request: &Value, result: Value) -> Result<Value> {
    if request.get("request_id").and_then(Value::as_str).unwrap_or_default().is_empty() {
        return Ok(result);
    }
    let recorded = store.write(|tx| {
        // Two retries of one request: the first to finish wins.
        if let Some(stored) = commands::replay(tx, command, request)? {
            return Ok(stored);
        }
        commands::remember(tx, command, request, &result)?;
        Ok(result.clone())
    });
    match recorded {
        Ok(value) => Ok(value),
        Err(error) => {
            eprintln!("hgs overmind: could not record the forwarded result: {error:#}");
            Ok(result)
        }
    }
}

/// The session that receives a rework or an answer: the task's assignee.
fn assignee_on_another_machine(ctx: &mut Ctx, task: &str) -> Result<Option<SessionRef>> {
    let assignee = ctx.store.read(|tx| Ok(load(tx, &resolve(tx, task)?)?.assignee))?;
    Ok(assignee.filter(|session| session.machine != ctx.store.machine()))
}

pub fn send(ctx: &mut Ctx, request: Value) -> Result<Value> {
    send_on(ctx, request, false)
}

/// `send`; `local_only` refuses a session on another machine instead of forwarding.
pub fn send_on(ctx: &mut Ctx, request: Value, local_only: bool) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: SendRequest = parse(&request)?;
    let session = local_session("session", Some(req.session.clone().into()), ctx.store.machine())?.expect("a session was given");
    if let Some(result) = ctx.store.read(|tx| commands::replay(tx, "send", &request))? {
        return Ok(result);
    }
    if session.machine != ctx.store.machine() {
        return route(ctx, "send", &request, &req.task, &session, local_only);
    }
    let request_id = if req.request_id.is_empty() { uuid::Uuid::new_v4().to_string() } else { req.request_id.clone() };
    let dir = ctx.store.dir().to_owned();
    let machine = ctx.store.machine().to_owned();
    // The gates and the session checks refuse before images this machine lacks
    // are fetched, so a refused request reaches no other machine. A claim this
    // request already wrote is resumed without fetching and without the session
    // checks: its text may be typed already, and `submit` decides from its receipt.
    let (fresh, loaded) = ctx.store.read(|tx| {
        let task = load_writable(tx, &resolve(tx, &req.task)?)?;
        let fresh = pending_claim(tx, &task, &request_id)?.is_none();
        if fresh {
            send_gates(tx, &machine, &workflow(tx, &task.project)?, &task, &session, &req.confirm)?;
        }
        Ok((fresh, task))
    })?;
    let target = target_for(ctx.store, &session, &req.confirm, !fresh)?;
    if fresh {
        super::remote::ensure_blobs(ctx.store, &loaded)?;
    }
    // Everything that can refuse runs before the claim is written.
    let (uuid, attachments, resumed) = ctx.store.read(|tx| {
        let uuid = resolve(tx, &req.task)?;
        let task = load_writable(tx, &uuid)?;
        let resumed = pending_claim(tx, &task, &request_id)?.is_some();
        if !resumed {
            send_gates(tx, &machine, &workflow(tx, &task.project)?, &task, &session, &req.confirm)?;
        }
        Ok((uuid, files(&dir, &task), resumed))
    })?;
    let attachments = if resumed { attachments } else { Ok(attachments?) };
    // The claim was settled by a retry meanwhile: this request claims afresh.
    let target = if fresh || resumed { target } else { target_for(ctx.store, &session, &req.confirm, false)? };
    let actor = ctx.actor.clone();
    let (claim, text) = ctx.store.write(|tx| {
        let task = load_writable(tx, &uuid)?;
        let claim = match pending_claim(tx, &task, &request_id)? {
            Some(claim) => claim,
            None => {
                let to = send_gates(tx, &machine, &workflow(tx, &task.project)?, &task, &session, &req.confirm)?;
                transition(tx, &actor, &task, Action::Claim, &to, BTreeMap::new(), Some(session.clone()), vec![], &request_id)?
            }
        };
        // A text that `hgs send` would refuse rolls the claim back.
        let text = render(tx, &load(tx, &uuid)?, "dispatch", "", &session)?;
        Ok((claim, text))
    })?;
    let outcome = match (target, attachments) {
        (Ok(target), Ok(files)) => deliver(&session, &target, &request_id, &text, files),
        (Err(error), _) => Outcome::Uncertain(format!("{error:#}")),
        (_, Err(error)) => Outcome::Uncertain(format!("{error:#}")),
    };
    settle_send(ctx, &request, &uuid, &claim, &session, outcome)
}

/// Records how a send ended. A definite failure returns the task to its queue
/// and refuses; the other outcomes are remembered for the request.
fn settle_send(ctx: &mut Ctx, request: &Value, uuid: &str, claim: &Event, session: &SessionRef, outcome: Outcome) -> Result<Value> {
    let actor = ctx.actor.clone();
    let result = ctx.store.write(|tx| {
        // Two retries of one request: the first to settle wins.
        if let Some(result) = commands::replay(tx, "send", request)? {
            return Ok(result);
        }
        let task = load(tx, uuid)?;
        let open = unsettled(tx, &task, claim)?;
        match &outcome {
            _ if !open => {}
            Outcome::Sent => journal(tx, &actor, &task, "[sent]", &session.name, &claim.id)?,
            Outcome::Uncertain(why) => {
                journal(tx, &actor, &task, "[sent?]", why, &claim.id)?;
                flag(tx, &actor, &task, "delivery_uncertain", true)?;
            }
            Outcome::NotSent(why) => {
                let back = rule_target(&workflow(tx, &task.project)?, Action::Unclaim, &task.stage)
                    .ok_or_else(|| Refusal::new("transition_not_allowed", "the claim cannot be taken back"))?;
                transition(tx, &actor, &task, Action::Unclaim, &back, reason(why), None, vec![], "")?;
            }
        }
        let task = load(tx, uuid)?;
        let result = json!({"ok": true, "delivery": outcome.name(), "reason": outcome.reason(), "task": view(&task, &reference(tx, uuid)?, true)});
        if !matches!(outcome, Outcome::NotSent(_)) {
            commands::remember(tx, "send", request, &result)?;
        }
        Ok(result)
    })?;
    if let Outcome::NotSent(why) = outcome {
        return Err(Refusal::new("not_delivered", format!("not delivered: {why}")).into());
    }
    Ok(result)
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ResolveRequest {
    /// Read by `commands::write` from the raw request.
    #[serde(default, rename = "request_id")]
    _request_id: String,
    task: String,
    delivered: bool,
}

/// What an uncertain delivery sent, from the latest `[sent?]` entry: the one
/// the `delivery_uncertain` flag belongs to.
enum Uncertain {
    /// A send: the entry references its claim.
    Claim,
    /// A review: the entry names review.
    Review,
    /// An answer or rework, or no entry at all.
    Other,
}

fn uncertain_delivery(tx: &Tx, task: &Task) -> Result<Uncertain> {
    let events = tx.load(&Filter::Task(task.uuid.clone()))?;
    let latest = events
        .iter()
        .rev()
        .find(|e| e.kind == "journal" && e.payload["marker"] == "[sent?]" && !task.rejections.contains_key(&e.id));
    let Some(entry) = latest else {
        return Ok(Uncertain::Other);
    };
    let settles = entry.payload["transition"].as_str().unwrap_or_default();
    if !settles.is_empty() && events.iter().any(|e| e.id == settles && e.kind == "transition" && e.payload["action"] == "claim") {
        return Ok(Uncertain::Claim);
    }
    if entry.payload["text"].as_str().is_some_and(|text| text.starts_with("review: ")) {
        return Ok(Uncertain::Review);
    }
    Ok(Uncertain::Other)
}

/// `delivered: false` undoes only what the uncertain delivery started: a send
/// takes its claim back, a review clears the reviewer, and an answer or rework
/// is only noted.
pub fn resolve_delivery(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: ResolveRequest = parse(&request)?;
    commands::write(ctx, "resolve-delivery", &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load_writable(tx, &uuid)?;
        if !task.flags.contains("delivery_uncertain") {
            return refuse("not_uncertain", "this task has no unconfirmed delivery");
        }
        let sent = uncertain_delivery(tx, &task)?;
        flag(tx, actor, &task, "delivery_uncertain", false)?;
        let task = load(tx, &uuid)?;
        let lost = "not delivered, confirmed by a person";
        if req.delivered {
            journal(tx, actor, &task, "[sent]", "delivery confirmed by a person", "")?;
            return commands::task_result(tx, &uuid);
        }
        match sent {
            Uncertain::Claim => match rule_target(&workflow(tx, &task.project)?, Action::Unclaim, &task.stage) {
                Some(back) if task.assignee.is_some() => {
                    transition(tx, actor, &task, Action::Unclaim, &back, reason(lost), None, vec![], "")?;
                }
                _ => journal(tx, actor, &task, "[comment]", lost, "")?,
            },
            Uncertain::Review => {
                simple(tx, actor, &task, "reviewer_set", json!({"session": null}), "")?;
            }
            Uncertain::Other => journal(tx, actor, &task, "[comment]", lost, "")?,
        }
        commands::task_result(tx, &uuid)
    })
}
/// Review goes to another session, for a task in a review stage.
fn review_gates(tx: &Tx, task: &Task, session: &SessionRef) -> Result<()> {
    if task.labels.contains("epic") {
        return refuse("epic", "an epic is a container and is never reviewed");
    }
    if workflow(tx, &task.project)?.role(&task.stage) != Some(Role::Review) {
        return refuse("transition_not_allowed", "only a task in review can be sent for review");
    }
    if task.assignee.as_ref() == Some(session) {
        return refuse("not_independent", "the session that did the work cannot review it");
    }
    Ok(())
}

fn assignee_of(task: &Task) -> Result<SessionRef> {
    task.assignee
        .clone()
        .ok_or_else(|| Refusal::new("transition_not_allowed", "this task is not assigned to a session").into())
}

/// The session that rework or an answer goes to.
fn assignee_for(task: &Task, purpose: &str) -> Result<SessionRef> {
    let assignee = assignee_of(task)?;
    match purpose {
        "rework" if !task.labels.contains("review-failed") => refuse("not_reviewed", "rework follows a review that asked for changes"),
        "answer" if task.resume_stage.is_empty() => refuse("no_question", "this task is not waiting for an answer"),
        _ => Ok(assignee),
    }
}

/// A retry after typing: the input receipt of this request exists, so the
/// gates, the session checks and the intent were done by the first attempt.
fn typed_before(request_id: &str) -> bool {
    !request_id.is_empty() && crate::state::input_receipt_exists(request_id)
}

fn input_id(request_id: &str) -> String {
    if request_id.is_empty() { uuid::Uuid::new_v4().to_string() } else { request_id.to_owned() }
}

/// The session's delivery target after the session checks. A retry after
/// typing skips the checks and keeps a lookup error for `deliver_resumed`.
fn target_for(store: &Store, session: &SessionRef, confirmed: &Confirmed, resumed: bool) -> Result<Result<DeliveryTarget>> {
    let target = local_target(store, session);
    if resumed {
        return Ok(target);
    }
    let target = target?;
    check_session(session, &target, confirmed)?;
    Ok(Ok(target))
}

/// `rework-send` and `answer` name the receiving session only by the task, so
/// their refusals about it use the field `task`, here as on a forwarding caller.
fn about_task<T>(result: Result<T>) -> Result<T> {
    result.map_err(|error| match error.downcast::<Refusal>() {
        Ok(mut refusal) => {
            if refusal.field == "session" {
                refusal.field = "task".into();
            }
            refusal.into()
        }
        Err(error) => error,
    })
}

fn deliver_resumed(session: &SessionRef, target: Result<DeliveryTarget>, attachments: Result<Vec<Value>>, text: Result<String>, input: &str) -> Outcome {
    match (target, attachments, text) {
        (Ok(target), Ok(files), Ok(text)) => deliver(session, &target, input, &text, files),
        (Err(error), _, _) | (_, Err(error), _) | (_, _, Err(error)) => Outcome::Uncertain(format!("{error:#}")),
    }
}

/// A rendering failure refuses a fresh request but not a retry after typing,
/// whose text may be typed already.
fn lenient<T>(result: Result<T>, resumed: bool) -> Result<Result<T>> {
    if resumed {
        Ok(result)
    } else {
        Ok(Ok(result?))
    }
}

/// Records a delivery that is not a claim; `undo` runs when it surely failed.
#[allow(clippy::too_many_arguments)]
fn settle(
    ctx: &mut Ctx,
    command: &str,
    request: &Value,
    uuid: &str,
    purpose: &str,
    session: &SessionRef,
    outcome: Outcome,
    settles: &str,
    hint: &str,
    undo: impl FnOnce(&mut Tx, &Actor, &Task) -> Result<()>,
) -> Result<Value> {
    let actor = ctx.actor.clone();
    let result = ctx.store.write(|tx| {
        // Two retries of one request: the first to settle wins.
        if let Some(result) = commands::replay(tx, command, request)? {
            return Ok(result);
        }
        let task = load(tx, uuid)?;
        match &outcome {
            Outcome::Sent => journal(tx, &actor, &task, "[sent]", &format!("{purpose}: {}", session.name), settles)?,
            Outcome::Uncertain(why) => {
                journal(tx, &actor, &task, "[sent?]", &format!("{purpose}: {why}"), settles)?;
                flag(tx, &actor, &task, "delivery_uncertain", true)?;
            }
            Outcome::NotSent(_) => undo(tx, &actor, &task)?,
        }
        let task = load(tx, uuid)?;
        let result = json!({"ok": true, "delivery": outcome.name(), "reason": outcome.reason(), "task": view(&task, &reference(tx, uuid)?, true)});
        if !matches!(outcome, Outcome::NotSent(_)) {
            commands::remember(tx, command, request, &result)?;
        }
        Ok(result)
    })?;
    if let Outcome::NotSent(why) = outcome {
        return Err(Refusal::new("not_delivered", format!("not delivered: {why}{hint}")).into());
    }
    Ok(result)
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ReviewSendRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    session: NamedSession,
    #[serde(default)]
    confirm: Confirmed,
}

pub fn review_send(ctx: &mut Ctx, request: Value) -> Result<Value> {
    review_send_on(ctx, request, false)
}

/// `review-send`; `local_only` as in `send_on`.
pub fn review_send_on(ctx: &mut Ctx, request: Value, local_only: bool) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: ReviewSendRequest = parse(&request)?;
    let session = local_session("session", Some(req.session.clone().into()), ctx.store.machine())?.expect("a session was given");
    if let Some(result) = ctx.store.read(|tx| commands::replay(tx, "review-send", &request))? {
        return Ok(result);
    }
    if session.machine != ctx.store.machine() {
        return route(ctx, "review-send", &request, &req.task, &session, local_only);
    }
    let resumed = typed_before(&req.request_id);
    let dir = ctx.store.dir().to_owned();
    // The gates and the session checks refuse before images are fetched.
    let gated = if resumed {
        None
    } else {
        Some(ctx.store.read(|tx| {
            let task = load_writable(tx, &resolve(tx, &req.task)?)?;
            review_gates(tx, &task, &session)?;
            Ok(task)
        })?)
    };
    let target = target_for(ctx.store, &session, &req.confirm, resumed)?;
    if let Some(task) = &gated {
        super::remote::ensure_blobs(ctx.store, task)?;
    }
    let (uuid, attachments) = ctx.store.read(|tx| {
        let uuid = resolve(tx, &req.task)?;
        let task = load_writable(tx, &uuid)?;
        if !resumed {
            review_gates(tx, &task, &session)?;
        }
        Ok((uuid, files(&dir, &task)))
    })?;
    let attachments = if resumed { attachments } else { Ok(attachments?) };
    let actor = ctx.actor.clone();
    let text = ctx.store.write(|tx| {
        let task = load_writable(tx, &uuid)?;
        if !resumed {
            review_gates(tx, &task, &session)?;
            simple(tx, &actor, &task, "reviewer_set", json!({"session": session}), "")?;
        }
        lenient(render(tx, &load(tx, &uuid)?, "review", "", &session), resumed)
    })?;
    let outcome = deliver_resumed(&session, target, attachments, text, &input_id(&req.request_id));
    settle(ctx, "review-send", &request, &uuid, "review", &session, outcome, "", "", |tx, actor, task| {
        simple(tx, actor, task, "reviewer_set", json!({"session": null}), "")?;
        Ok(())
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ReworkRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    #[serde(default)]
    confirm: Confirmed,
}

pub fn rework_send(ctx: &mut Ctx, request: Value) -> Result<Value> {
    rework_send_on(ctx, request, false)
}

/// `rework-send`; `local_only` as in `send_on`.
pub fn rework_send_on(ctx: &mut Ctx, request: Value, local_only: bool) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: ReworkRequest = parse(&request)?;
    if let Some(result) = ctx.store.read(|tx| commands::replay(tx, "rework-send", &request))? {
        return Ok(result);
    }
    if let Some(session) = assignee_on_another_machine(ctx, &req.task)? {
        return route(ctx, "rework-send", &request, &req.task, &session, local_only);
    }
    let resumed = typed_before(&req.request_id);
    let dir = ctx.store.dir().to_owned();
    // The gates and the session checks refuse before images are fetched.
    let checked = if resumed {
        None
    } else {
        let (task, session) = ctx.store.read(|tx| {
            let task = load_writable(tx, &resolve(tx, &req.task)?)?;
            let session = assignee_for(&task, "rework")?;
            Ok((task, session))
        })?;
        let target = about_task(target_for(ctx.store, &session, &req.confirm, false))?;
        super::remote::ensure_blobs(ctx.store, &task)?;
        Some((session, target))
    };
    let (uuid, session, text, attachments) = ctx.store.read(|tx| {
        let uuid = resolve(tx, &req.task)?;
        let task = load_writable(tx, &uuid)?;
        let session = if resumed { assignee_of(&task)? } else { assignee_for(&task, "rework")? };
        let text = lenient(render(tx, &task, "rework", "", &session), resumed)?;
        Ok((uuid, session, text, files(&dir, &task)))
    })?;
    let attachments = if resumed { attachments } else { Ok(attachments?) };
    // The assignee changed meanwhile: the new one is checked too.
    let target = match checked {
        Some((checked, target)) if checked == session => target,
        _ => about_task(target_for(ctx.store, &session, &req.confirm, resumed))?,
    };
    let outcome = deliver_resumed(&session, target, attachments, text, &input_id(&req.request_id));
    settle(ctx, "rework-send", &request, &uuid, "rework", &session, outcome, "", "", |_, _, _| Ok(()))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct AnswerRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    answer: String,
    #[serde(default)]
    confirm: Confirmed,
}

pub fn answer(ctx: &mut Ctx, request: Value) -> Result<Value> {
    answer_on(ctx, request, false)
}

/// `answer`; `local_only` as in `send_on`.
pub fn answer_on(ctx: &mut Ctx, request: Value, local_only: bool) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: AnswerRequest = parse(&request)?;
    if let Some(result) = ctx.store.read(|tx| commands::replay(tx, "answer", &request))? {
        return Ok(result);
    }
    if let Some(session) = assignee_on_another_machine(ctx, &req.task)? {
        return route(ctx, "answer", &request, &req.task, &session, local_only);
    }
    let input = input_id(&req.request_id);
    let resumed = typed_before(&req.request_id);
    // An answer this request recorded but did not settle is sent again.
    let (uuid, session, pending) = ctx.store.read(|tx| {
        let uuid = resolve(tx, &req.task)?;
        let task = load_writable(tx, &uuid)?;
        let pending = pending_transition(tx, &task, &req.request_id, "answer")?;
        // Repeating the request sends the recorded answer; another text is another request.
        if pending.as_ref().is_some_and(|event| event.payload["fields"]["answer"] != commands::normalize(&req.answer).as_str()) {
            return refuse("request_reused", "this request_id was already used for a different request");
        }
        let session = if resumed || pending.is_some() { assignee_of(&task)? } else { assignee_for(&task, "answer")? };
        Ok((uuid, session, pending))
    })?;
    let target = about_task(target_for(ctx.store, &session, &req.confirm, resumed))?;
    let actor = ctx.actor.clone();
    let recorded = pending.as_ref().map(|e| e.payload["fields"]["answer"].as_str().unwrap_or_default().to_owned());
    let (typed, settles) = ctx.store.write(|tx| {
        let task = load_writable(tx, &uuid)?;
        let (text, settles) = match (&pending, &recorded) {
            (Some(event), Some(text)) => (text.clone(), event.id.clone()),
            _ if resumed => (commands::normalize(&req.answer), String::new()),
            _ => {
                let text = commands::normalize(&req.answer);
                let fields = BTreeMap::from([("answer".to_owned(), text.clone())]);
                let event = transition(tx, &actor, &task, Action::Answer, super::workflow::RESUME, fields, None, vec![], &input)?;
                (text, event.id)
            }
        };
        Ok((lenient(render(tx, &load(tx, &uuid)?, "answer", &text, &session), resumed)?, settles))
    })?;
    let outcome = deliver_resumed(&session, target, Ok(Vec::new()), typed, &input);
    // The answer stays recorded; the person can repeat the request to send it again.
    let hint = format!("; the answer is recorded, repeat this answer with request_id {input} to send it again");
    settle(ctx, "answer", &request, &uuid, "answer", &session, outcome, &settles, &hint, |tx, actor, task| {
        journal(tx, actor, task, "[comment]", &format!("the answer was not delivered to {}", session.name), "")
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::overmind::commands::{create, link};
    use serde_json::json;

    const MACHINE: &str = "11111111-1111-4111-8111-111111111111";

    fn new_task(store: &mut Store, stage: &str) -> String {
        let mut ctx = Ctx { store, actor: Actor::human(MACHINE), cwd: None };
        let created = create(&mut ctx, json!({"project": "p", "prompt": "Do it", "stage": stage})).unwrap();
        created["task"]["uuid"].as_str().unwrap().to_owned()
    }
    fn session(name: &str) -> SessionRef {
        SessionRef { machine: MACHINE.into(), name: name.into() }
    }
    fn claim(store: &mut Store, uuid: &str, name: &str, request_id: &str) -> Event {
        store
            .write(|tx| {
                let task = load(tx, uuid)?;
                transition(tx, &Actor::human(MACHINE), &task, Action::Claim, "Design", BTreeMap::new(), Some(session(name)), vec![], request_id)
            })
            .unwrap()
    }
    fn code(result: Result<impl std::fmt::Debug>) -> String {
        result.unwrap_err().downcast::<Refusal>().unwrap().code
    }

    #[test]
    fn a_forwarded_result_is_recorded_only_for_a_request_id_and_never_fails_the_host() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let receipts = || -> i64 {
            let conn = rusqlite::Connection::open(dir.path().join("overmind.sqlite3")).unwrap();
            conn.query_row("SELECT COUNT(*) FROM receipts", [], |row| row.get(0)).unwrap()
        };
        let result = json!({"ok": true, "status": "submitted"});
        let anonymous = json!({"task": "t", "session": {"machine": MACHINE, "name": "w1"}});
        assert_eq!(record_forwarded(&mut store, "send", &anonymous, result.clone()).unwrap(), result);
        assert_eq!(receipts(), 0, "no request_id, no receipt");

        let identified = json!({"task": "t", "request_id": "33333333-3333-4333-8333-333333333333"});
        assert_eq!(record_forwarded(&mut store, "send", &identified, result.clone()).unwrap(), result);
        assert_eq!(receipts(), 1);

        // Another process holds the write lock: the receipt cannot be written.
        let other = rusqlite::Connection::open(dir.path().join("overmind.sqlite3")).unwrap();
        other.execute_batch("BEGIN IMMEDIATE").unwrap();
        store.wait_for_writers(std::time::Duration::from_millis(20)).unwrap();
        let again = json!({"task": "t", "request_id": "44444444-4444-4444-8444-444444444444"});
        assert_eq!(record_forwarded(&mut store, "send", &again, result.clone()).unwrap(), result);
        // Without a request_id nothing is written, so another writer is never waited for.
        store.wait_for_writers(std::time::Duration::from_secs(5)).unwrap();
        let started = std::time::Instant::now();
        assert_eq!(record_forwarded(&mut store, "send", &anonymous, result.clone()).unwrap(), result);
        assert!(started.elapsed() < std::time::Duration::from_secs(2), "waited for the other writer");
        other.execute_batch("ROLLBACK").unwrap();
        assert_eq!(receipts(), 1, "the failed receipt was not written");
    }

    #[test]
    fn gates_refuse_epics_and_ask_to_confirm_the_rest() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let first = new_task(&mut store, "Queue");
        let second = new_task(&mut store, "Queue");
        let none = Confirmed::new();
        let gates = |store: &mut Store, uuid: &str, confirmed: &Confirmed| {
            store.read(|tx| {
                let task = load(tx, uuid)?;
                send_gates(tx, MACHINE, &workflow(tx, "p")?, &task, &session("w1"), confirmed)
            })
        };
        assert_eq!(gates(&mut store, &first, &none).unwrap(), "Design");
        claim(&mut store, &first, "w1", "");
        assert_eq!(code(gates(&mut store, &second, &none)), "wip_exceeded");
        let wip = Confirmed::from(["wip_exceeded".to_string()]);
        assert!(gates(&mut store, &second, &wip).is_ok());
        let third = new_task(&mut store, "Queue");
        let mut ctx = Ctx { store: &mut store, actor: Actor::human(MACHINE), cwd: None };
        link(&mut ctx, json!({"task": third, "relation": "follows", "target": second, "present": true})).unwrap();
        assert_eq!(code(gates(&mut store, &third, &wip)), "predecessors_open");
        let refusal = gates(&mut store, &third, &wip).unwrap_err().downcast::<Refusal>().unwrap();
        assert_eq!((refusal.field.as_str(), refusal.state.as_str()), ("confirm", "missing"));
        assert_eq!(code(gates(&mut store, &first, &wip)), "transition_not_allowed");

        let reserved = new_task(&mut store, "Queue");
        let mut ctx = Ctx { store: &mut store, actor: Actor::human(MACHINE), cwd: None };
        commands::pin(&mut ctx, json!({"task": reserved, "session": {"machine": MACHINE, "name": "w9"}})).unwrap();
        assert_eq!(code(gates(&mut store, &reserved, &wip)), "pinned_elsewhere");
        let both = Confirmed::from(["wip_exceeded".to_string(), "pinned_elsewhere".to_string()]);
        assert!(gates(&mut store, &reserved, &both).is_ok());

        let epic = new_task(&mut store, "Queue");
        store
            .write(|tx| {
                let task = load(tx, &epic)?;
                commands::simple(tx, &Actor::human(MACHINE), &task, "label_set", json!({"label": "epic", "present": true}), "")?;
                Ok(())
            })
            .unwrap();
        assert_eq!(code(gates(&mut store, &epic, &both)), "epic");
    }

    #[test]
    fn active_tasks_count_working_and_waiting_tasks_in_every_project() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let in_project = |store: &mut Store, project: &str| -> String {
            let mut ctx = Ctx { store, actor: Actor::human(MACHINE), cwd: None };
            let created = create(&mut ctx, json!({"project": project, "prompt": "Do it", "stage": "Queue"})).unwrap();
            created["task"]["uuid"].as_str().unwrap().to_owned()
        };
        let designing = in_project(&mut store, "p");
        claim(&mut store, &designing, "w1", "");
        let building = in_project(&mut store, "q");
        claim(&mut store, &building, "w1", "");
        agent_step(&mut store, &building, Action::Advance, "Build", &[("spec", "s")]);
        let reviewed = in_project(&mut store, "q");
        claim(&mut store, &reviewed, "w1", "");
        agent_step(&mut store, &reviewed, Action::Advance, "Build", &[("spec", "s")]);
        agent_step(&mut store, &reviewed, Action::Advance, "Review", &[("worklog", "w"), ("evidence", "e")]);
        let asking = in_project(&mut store, "p");
        claim(&mut store, &asking, "w1", "");
        agent_step(&mut store, &asking, Action::Advance, "Build", &[("spec", "s")]);
        agent_step(&mut store, &asking, Action::CallHuman, "Your Call", &[("question", "q")]);
        let elsewhere = in_project(&mut store, "p");
        claim(&mut store, &elsewhere, "w2", "");
        let deleted = in_project(&mut store, "q");
        claim(&mut store, &deleted, "w1", "");
        let mut ctx = Ctx { store: &mut store, actor: Actor::human(MACHINE), cwd: None };
        commands::delete(&mut ctx, json!({"task": deleted})).unwrap();
        assert_eq!(store.read(|tx| active_tasks(tx, &session("w1"))).unwrap(), 3);
    }

    #[test]
    fn outcomes_are_recorded_and_never_retyped() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let mut ctx = Ctx { store: &mut store, actor: Actor::human(MACHINE), cwd: None };
        let sent = new_task(ctx.store, "Queue");
        let c = claim(ctx.store, &sent, "w1", "");
        let request = json!({"task": sent});
        let result = settle_send(&mut ctx, &request, &sent, &c, &session("w1"), Outcome::Sent).unwrap();
        assert_eq!(result["delivery"], "sent");
        let journal = &result["task"]["journal"];
        assert!(journal.as_array().unwrap().iter().any(|j| j["marker"] == "[sent]" && j["text"].as_str().unwrap().contains(&c.id)));

        let uncertain = new_task(ctx.store, "Queue");
        let c = claim(ctx.store, &uncertain, "w2", "");
        let result = settle_send(&mut ctx, &json!({"task": uncertain}), &uncertain, &c, &session("w2"), Outcome::Uncertain("lost".into())).unwrap();
        assert_eq!(result["delivery"], "uncertain");
        assert!(result["task"]["flags"].as_array().unwrap().contains(&json!("delivery_uncertain")));

        let failed = new_task(ctx.store, "Queue");
        let c = claim(ctx.store, &failed, "w3", "");
        let error = settle_send(&mut ctx, &json!({"task": failed}), &failed, &c, &session("w3"), Outcome::NotSent("stopped".into())).unwrap_err();
        assert_eq!(error.downcast::<Refusal>().unwrap().code, "not_delivered");
        let task = ctx.store.read(|tx| load(tx, &failed)).unwrap();
        assert_eq!((task.stage.as_str(), task.assignee.is_none()), ("Queue", true));
        assert!(task.journal.iter().any(|j| j.marker == "[unclaim]" && j.text.contains("stopped")));
    }

    #[test]
    fn a_pending_claim_is_found_by_its_request() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let uuid = new_task(&mut store, "Queue");
        let request_id = "0b9c5c1e-34c4-4d5e-9a64-1d2a3b4c5d6e";
        let c = claim(&mut store, &uuid, "w1", request_id);
        let found = store.read(|tx| pending_claim(tx, &load(tx, &uuid)?, request_id)).unwrap();
        assert_eq!(found.map(|e| e.id), Some(c.id.clone()));
        assert!(store.read(|tx| pending_claim(tx, &load(tx, &uuid)?, "other")).unwrap().is_none());
        let mut ctx = Ctx { store: &mut store, actor: Actor::human(MACHINE), cwd: None };
        settle_send(&mut ctx, &json!({"task": uuid}), &uuid, &c, &session("w1"), Outcome::Sent).unwrap();
        assert!(store.read(|tx| pending_claim(tx, &load(tx, &uuid)?, request_id)).unwrap().is_none());
    }

    #[test]
    fn a_person_resolves_an_uncertain_delivery() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let mut ctx = Ctx { store: &mut store, actor: Actor::human(MACHINE), cwd: None };
        let plain = new_task(ctx.store, "Queue");
        assert_eq!(code(resolve_delivery(&mut ctx, json!({"task": plain, "delivered": true}))), "not_uncertain");
        for delivered in [true, false] {
            let uuid = new_task(ctx.store, "Queue");
            let c = claim(ctx.store, &uuid, "w1", "");
            settle_send(&mut ctx, &json!({"task": uuid}), &uuid, &c, &session("w1"), Outcome::Uncertain("?".into())).unwrap();
            let result = resolve_delivery(&mut ctx, json!({"task": uuid, "delivered": delivered})).unwrap();
            assert!(!result["task"]["flags"].as_array().unwrap().contains(&json!("delivery_uncertain")));
            assert_eq!(result["task"]["stage"], if delivered { "Design" } else { "Queue" });
        }
    }

    /// A task in Build assigned to w1.
    fn in_build(store: &mut Store) -> String {
        let uuid = new_task(store, "Queue");
        claim(store, &uuid, "w1", "");
        agent_step(store, &uuid, Action::Advance, "Build", &[("spec", "s")]);
        uuid
    }
    fn agent_step(store: &mut Store, uuid: &str, action: Action, to: &str, fields: &[(&str, &str)]) -> Event {
        let fields = fields.iter().map(|(k, v)| (k.to_string(), v.to_string())).collect();
        store.write(|tx| transition(tx, &Actor::agent(MACHINE, "w1"), &load(tx, uuid)?, action, to, fields, None, vec![], "")).unwrap()
    }
    fn not_delivered(ctx: &mut Ctx, uuid: &str) -> Value {
        let result = resolve_delivery(ctx, json!({"task": uuid, "delivered": false})).unwrap();
        assert!(!result["task"]["flags"].as_array().unwrap().contains(&json!("delivery_uncertain")));
        result["task"].clone()
    }

    fn uncertain() -> Outcome {
        Outcome::Uncertain("?".into())
    }
    fn last_entry(task: &Value) -> Value {
        task["journal"].as_array().unwrap().last().unwrap().clone()
    }

    #[test]
    fn a_lost_review_clears_only_the_reviewer() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let review = in_build(&mut store);
        agent_step(&mut store, &review, Action::Advance, "Review", &[("worklog", "w"), ("evidence", "e")]);
        store
            .write(|tx| commands::simple(tx, &Actor::human(MACHINE), &load(tx, &review)?, "reviewer_set", json!({"session": session("w2")}), ""))
            .unwrap();
        let mut ctx = Ctx { store: &mut store, actor: Actor::human(MACHINE), cwd: None };
        settle(&mut ctx, "review-send", &json!({"task": review}), &review, "review", &session("w2"), uncertain(), "", "", |_, _, _| Ok(())).unwrap();
        let task = not_delivered(&mut ctx, &review);
        assert_eq!((task["stage"].as_str(), task["assignee"]["name"].as_str(), &task["reviewer"]), (Some("Review"), Some("w1"), &Value::Null));
    }

    #[test]
    fn a_lost_answer_stays_recorded_and_keeps_the_session() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let answered = in_build(&mut store);
        agent_step(&mut store, &answered, Action::CallHuman, "Your Call", &[("question", "q")]);
        let answer = store
            .write(|tx| {
                let fields = BTreeMap::from([("answer".to_owned(), "REST".to_owned())]);
                transition(tx, &Actor::human(MACHINE), &load(tx, &answered)?, Action::Answer, super::super::workflow::RESUME, fields, None, vec![], "")
            })
            .unwrap();
        let mut ctx = Ctx { store: &mut store, actor: Actor::human(MACHINE), cwd: None };
        settle(&mut ctx, "answer", &json!({"task": answered}), &answered, "answer", &session("w1"), uncertain(), &answer.id, "", |_, _, _| Ok(())).unwrap();
        let task = not_delivered(&mut ctx, &answered);
        assert_eq!((task["stage"].as_str(), task["assignee"]["name"].as_str()), (Some("Build"), Some("w1")));
        assert_eq!(last_entry(&task)["marker"], "[comment]");
        assert!(last_entry(&task)["text"].as_str().unwrap().contains("not delivered"));
        assert!(task["journal"].as_array().unwrap().iter().any(|j| j["marker"] == "[answer]" && j["text"] == "REST"));
    }

    #[test]
    fn a_lost_rework_only_records_a_comment() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let rework = in_build(&mut store);
        let mut ctx = Ctx { store: &mut store, actor: Actor::human(MACHINE), cwd: None };
        settle(&mut ctx, "rework-send", &json!({"task": rework}), &rework, "rework", &session("w1"), uncertain(), "", "", |_, _, _| Ok(())).unwrap();
        let task = not_delivered(&mut ctx, &rework);
        assert_eq!((task["stage"].as_str(), task["assignee"]["name"].as_str()), (Some("Build"), Some("w1")));
        assert_eq!(last_entry(&task)["marker"], "[comment]");
    }

    #[test]
    fn review_needs_another_session_and_a_task_in_review() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let uuid = new_task(&mut store, "Queue");
        claim(&mut store, &uuid, "w1", "");
        let gate = |store: &mut Store, name: &str| store.read(|tx| review_gates(tx, &load(tx, &uuid)?, &session(name)));
        assert_eq!(code(gate(&mut store, "w2")), "transition_not_allowed");
        store
            .write(|tx| {
                let agent = Actor::agent(MACHINE, "w1");
                let task = load(tx, &uuid)?;
                transition(tx, &agent, &task, Action::Advance, "Build", BTreeMap::from([("spec".to_owned(), "s".to_owned())]), None, vec![], "")?;
                let task = load(tx, &uuid)?;
                let fields = BTreeMap::from([("worklog".to_owned(), "w".to_owned()), ("evidence".to_owned(), "e".to_owned())]);
                transition(tx, &agent, &task, Action::Advance, "Review", fields, None, vec![], "")
            })
            .unwrap();
        assert_eq!(code(gate(&mut store, "w1")), "not_independent");
        assert!(gate(&mut store, "w2").is_ok());
    }

    #[test]
    fn rework_and_answer_go_to_the_assignee() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let uuid = new_task(&mut store, "Queue");
        assert_eq!(code(store.read(|tx| assignee_for(&load(tx, &uuid)?, "rework"))), "transition_not_allowed");
        claim(&mut store, &uuid, "w1", "");
        assert_eq!(code(store.read(|tx| assignee_for(&load(tx, &uuid)?, "rework"))), "not_reviewed");
        assert_eq!(code(store.read(|tx| assignee_for(&load(tx, &uuid)?, "answer"))), "no_question");
    }
}
