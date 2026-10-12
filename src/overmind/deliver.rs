//! Sending tasks into sessions on this machine (spec Part 5). The gates and the
//! session are checked first, then the claim is written, the text is typed like
//! `hgs send`, and the outcome is recorded. Overmind never types a text twice:
//! the input request ID is the Overmind request ID.
use super::blobs;
use super::commands::{
    self, human_only, load, load_writable, local_session, parse, reference, refuse, resolve, simple, transition, view,
    workflow, Ctx,
};
use super::fold::{fold_many, Task};
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
        .ok_or_else(|| Refusal::field("unknown_session", "session", "invalid", format!("there is no session {} on this machine", session.name)).into())
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

pub fn render(tx: &Tx, task: &Task, name: &str, answer: &str) -> Result<String> {
    let template = templates::effective(tx, &task.project, name)?;
    let reference = reference(tx, &task.uuid)?;
    Ok(templates::render(&template, &templates::Vars { task, reference: &reference, answer, tools: templates::TOOLS })?)
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
    let events = tx.load(&Filter::All)?;
    let mut count = 0;
    for task in fold_many(&events).values().filter(|t| !t.deleted && t.assignee.as_ref() == Some(session)) {
        if matches!(workflow(tx, &task.project)?.role(&task.stage), Some(Role::Active | Role::NeedsHuman)) {
            count += 1;
        }
    }
    Ok(count)
}

/// The stage a send claims the task into, after the gates of Part 3.
fn send_gates(tx: &Tx, wf: &Workflow, task: &Task, session: &SessionRef, confirmed: &Confirmed) -> Result<String> {
    if task.labels.contains("epic") {
        return refuse("epic", "an epic is a container and is never sent to a session");
    }
    let to = rule_target(wf, Action::Claim, &task.stage)
        .ok_or_else(|| Refusal::new("transition_not_allowed", format!("a task in {} cannot be sent", task.stage)))?;
    if let Some(pinned) = task.pinned.as_ref().filter(|pinned| *pinned != session) {
        confirmable("pinned_elsewhere", confirmed, format!("this task is reserved for {}", pinned.name))?;
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
    if request_id.is_empty() {
        return Ok(None);
    }
    let events = tx.load(&Filter::Task(task.uuid.clone()))?;
    let Some(claim) = events.iter().find(|e| {
        e.id == task.head && e.kind == "transition" && e.request_id == request_id && e.payload["action"] == "claim"
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

/// A session as a person names it; no machine means this one.
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct NamedSession {
    #[serde(default)]
    machine: String,
    name: String,
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

pub fn send(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: SendRequest = parse(&request)?;
    let session = local_session("session", Some(SessionRef { machine: req.session.machine.clone(), name: req.session.name.clone() }), ctx.store.machine())?.expect("a session was given");
    if let Some(result) = ctx.store.read(|tx| commands::replay(tx, "send", &request))? {
        return Ok(result);
    }
    let request_id = if req.request_id.is_empty() { uuid::Uuid::new_v4().to_string() } else { req.request_id.clone() };
    let dir = ctx.store.dir().to_owned();
    // Everything that can refuse runs before the claim is written. A claim this
    // request already wrote is resumed without the session checks: its text may
    // be typed already, and `submit` decides from its receipt.
    let (uuid, attachments, resumed) = ctx.store.read(|tx| {
        let uuid = resolve(tx, &req.task)?;
        let task = load_writable(tx, &uuid)?;
        let resumed = pending_claim(tx, &task, &request_id)?.is_some();
        if !resumed {
            send_gates(tx, &workflow(tx, &task.project)?, &task, &session, &req.confirm)?;
        }
        Ok((uuid, files(&dir, &task), resumed))
    })?;
    let target = local_target(ctx.store, &session);
    let attachments = if resumed { attachments } else { Ok(attachments?) };
    let target = if resumed {
        target
    } else {
        let target = target?;
        check_session(&session, &target, &req.confirm)?;
        Ok(target)
    };
    let actor = ctx.actor.clone();
    let (claim, text) = ctx.store.write(|tx| {
        let task = load_writable(tx, &uuid)?;
        let claim = match pending_claim(tx, &task, &request_id)? {
            Some(claim) => claim,
            None => {
                let to = send_gates(tx, &workflow(tx, &task.project)?, &task, &session, &req.confirm)?;
                transition(tx, &actor, &task, Action::Claim, &to, BTreeMap::new(), Some(session.clone()), vec![], &request_id)?
            }
        };
        // A text that `hgs send` would refuse rolls the claim back.
        let text = render(tx, &load(tx, &uuid)?, "dispatch", "")?;
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

pub fn resolve_delivery(ctx: &mut Ctx, request: Value) -> Result<Value> {
    human_only(&ctx.actor)?;
    let req: ResolveRequest = parse(&request)?;
    commands::write(ctx, "resolve-delivery", &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load_writable(tx, &uuid)?;
        if !task.flags.contains("delivery_uncertain") {
            return refuse("not_uncertain", "this task has no unconfirmed delivery");
        }
        flag(tx, actor, &task, "delivery_uncertain", false)?;
        let task = load(tx, &uuid)?;
        let back = rule_target(&workflow(tx, &task.project)?, Action::Unclaim, &task.stage);
        match (req.delivered, back) {
            (true, _) => journal(tx, actor, &task, "[sent]", "delivery confirmed by a person", "")?,
            (false, Some(back)) if task.assignee.is_some() => {
                transition(tx, actor, &task, Action::Unclaim, &back, reason("not delivered, confirmed by a person"), None, vec![], "")?;
            }
            (false, _) => journal(tx, actor, &task, "[comment]", "not delivered, confirmed by a person", "")?,
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

fn deliver_resumed(session: &SessionRef, target: Result<DeliveryTarget>, attachments: Result<Vec<Value>>, input: &str, text: &str) -> Outcome {
    match (target, attachments) {
        (Ok(target), Ok(files)) => deliver(session, &target, input, text, files),
        (Err(error), _) | (_, Err(error)) => Outcome::Uncertain(format!("{error:#}")),
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
            Outcome::Sent => journal(tx, &actor, &task, "[sent]", &format!("{purpose}: {}", session.name), "")?,
            Outcome::Uncertain(why) => {
                journal(tx, &actor, &task, "[sent?]", &format!("{purpose}: {why}"), "")?;
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
        return Err(Refusal::new("not_delivered", format!("not delivered: {why}")).into());
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
    human_only(&ctx.actor)?;
    let req: ReviewSendRequest = parse(&request)?;
    let session = local_session("session", Some(SessionRef { machine: req.session.machine.clone(), name: req.session.name.clone() }), ctx.store.machine())?.expect("a session was given");
    if let Some(result) = ctx.store.read(|tx| commands::replay(tx, "review-send", &request))? {
        return Ok(result);
    }
    let resumed = typed_before(&req.request_id);
    let dir = ctx.store.dir().to_owned();
    let (uuid, attachments) = ctx.store.read(|tx| {
        let uuid = resolve(tx, &req.task)?;
        let task = load_writable(tx, &uuid)?;
        if !resumed {
            review_gates(tx, &task, &session)?;
        }
        Ok((uuid, files(&dir, &task)))
    })?;
    let attachments = if resumed { attachments } else { Ok(attachments?) };
    let target = target_for(ctx.store, &session, &req.confirm, resumed)?;
    let actor = ctx.actor.clone();
    let text = ctx.store.write(|tx| {
        let task = load_writable(tx, &uuid)?;
        if !resumed {
            review_gates(tx, &task, &session)?;
            simple(tx, &actor, &task, "reviewer_set", json!({"session": session}), "")?;
        }
        render(tx, &load(tx, &uuid)?, "review", "")
    })?;
    let outcome = deliver_resumed(&session, target, attachments, &input_id(&req.request_id), &text);
    settle(ctx, "review-send", &request, &uuid, "review", &session, outcome, |tx, actor, task| {
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
    human_only(&ctx.actor)?;
    let req: ReworkRequest = parse(&request)?;
    if let Some(result) = ctx.store.read(|tx| commands::replay(tx, "rework-send", &request))? {
        return Ok(result);
    }
    let resumed = typed_before(&req.request_id);
    let dir = ctx.store.dir().to_owned();
    let (uuid, session, text, attachments) = ctx.store.read(|tx| {
        let uuid = resolve(tx, &req.task)?;
        let task = load_writable(tx, &uuid)?;
        let session = if resumed { assignee_of(&task)? } else { assignee_for(&task, "rework")? };
        Ok((uuid, session, render(tx, &task, "rework", "")?, files(&dir, &task)))
    })?;
    let attachments = if resumed { attachments } else { Ok(attachments?) };
    let target = target_for(ctx.store, &session, &req.confirm, resumed)?;
    let outcome = deliver_resumed(&session, target, attachments, &input_id(&req.request_id), &text);
    settle(ctx, "rework-send", &request, &uuid, "rework", &session, outcome, |_, _, _| Ok(()))
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
    human_only(&ctx.actor)?;
    let req: AnswerRequest = parse(&request)?;
    if let Some(result) = ctx.store.read(|tx| commands::replay(tx, "answer", &request))? {
        return Ok(result);
    }
    let resumed = typed_before(&req.request_id);
    let text = commands::normalize(&req.answer);
    let (uuid, session) = ctx.store.read(|tx| {
        let uuid = resolve(tx, &req.task)?;
        let task = load_writable(tx, &uuid)?;
        let session = if resumed { assignee_of(&task)? } else { assignee_for(&task, "answer")? };
        Ok((uuid, session))
    })?;
    let target = target_for(ctx.store, &session, &req.confirm, resumed)?;
    let actor = ctx.actor.clone();
    let typed = ctx.store.write(|tx| {
        let task = load_writable(tx, &uuid)?;
        if !resumed {
            let fields = BTreeMap::from([("answer".to_owned(), text.clone())]);
            transition(tx, &actor, &task, Action::Answer, super::workflow::RESUME, fields, None, vec![], "")?;
        }
        render(tx, &load(tx, &uuid)?, "answer", &text)
    })?;
    let outcome = deliver_resumed(&session, target, Ok(Vec::new()), &input_id(&req.request_id), &typed);
    // The answer stays recorded; the person sees it was not delivered.
    settle(ctx, "answer", &request, &uuid, "answer", &session, outcome, |tx, actor, task| {
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
    fn gates_refuse_epics_and_ask_to_confirm_the_rest() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let first = new_task(&mut store, "Queue");
        let second = new_task(&mut store, "Queue");
        let none = Confirmed::new();
        let gates = |store: &mut Store, uuid: &str, confirmed: &Confirmed| {
            store.read(|tx| {
                let task = load(tx, uuid)?;
                send_gates(tx, &workflow(tx, "p")?, &task, &session("w1"), confirmed)
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
