//! Folding events into task state. Every machine applies the same events in
//! the same total order and therefore reaches the same state. Transitions are
//! compare-and-set against the task's head and are checked against the
//! workflow in effect at that point; a refused transition stays visible as
//! `[rejected]`.
use super::model::*;
use super::rank;
use super::workflow::{Action, Role, Workflow, RESUME};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::{BTreeMap, BTreeSet};

const INVALID_EVENT: &str = "this event cannot be read by this version";

pub const VERDICT_LABELS: [&str; 2] = ["reviewed", "review-failed"];

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Attachment {
    pub name: String,
    pub mime: String,
    pub bytes: u64,
    pub sha256: String,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct JournalEntry {
    pub marker: String,
    pub text: String,
    pub at: u64,
    pub actor: Actor,
    pub event: String,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Task {
    pub uuid: String,
    pub project: String,
    pub title: String,
    pub prompt: String,
    pub attachments: Vec<Attachment>,
    pub stage: String,
    pub priority: u8,
    pub rank: String,
    pub labels: BTreeSet<String>,
    pub pinned: Option<SessionRef>,
    pub assignee: Option<SessionRef>,
    pub reviewer: Option<SessionRef>,
    pub resume_stage: String,
    pub parent: String,
    pub follows: BTreeSet<String>,
    pub related: BTreeSet<String>,
    pub journal: Vec<JournalEntry>,
    pub flags: BTreeSet<String>,
    pub head: String,
    pub deleted: bool,
    pub read_only: bool,
    pub created_at: u64,
    pub updated_at: u64,
    pub created_by: Actor,
    #[serde(skip)]
    pub rejections: BTreeMap<String, Refusal>,
}

#[derive(Deserialize)]
struct Created {
    title: String,
    prompt: String,
    stage: String,
    #[serde(default = "default_priority")]
    priority: u8,
    #[serde(default)]
    rank: String,
    #[serde(default)]
    labels: BTreeSet<String>,
    #[serde(default)]
    pinned: Option<SessionRef>,
    #[serde(default)]
    parent: String,
    #[serde(default)]
    follows: BTreeSet<String>,
    #[serde(default)]
    related: BTreeSet<String>,
    #[serde(default)]
    attachments: Vec<Attachment>,
    #[serde(default)]
    fields: BTreeMap<String, String>,
}

fn default_priority() -> u8 {
    2
}

#[derive(Deserialize)]
struct Transition {
    action: Action,
    #[serde(default)]
    to: String,
    #[serde(default)]
    fields: BTreeMap<String, String>,
    #[serde(default)]
    session: Option<SessionRef>,
    #[serde(default)]
    children: Vec<String>,
    /// The task's rank in the target stage, so that it joins the end of that column.
    #[serde(default)]
    rank: String,
}

#[derive(Deserialize)]
struct WorkflowSet {
    workflow: Workflow,
    #[serde(default)]
    moves: BTreeMap<String, String>,
}

fn parse_workflow(event: &Event) -> Option<WorkflowSet> {
    if event.actor.kind != ActorKind::Human {
        return None;
    }
    let set: WorkflowSet = serde_json::from_value(event.payload.clone()).ok()?;
    set.workflow.validate().ok()?;
    Some(set)
}

/// The project's workflow after all `workflow_set` events in `events`.
pub fn project_workflow(events: &[&Event], project: &str) -> Workflow {
    let mut sorted: Vec<&&Event> = events
        .iter()
        .filter(|e| e.task.is_empty() && e.kind == "workflow_set" && e.project == project && e.v <= EVENT_VERSION)
        .collect();
    sorted.sort_by(|a, b| a.order().cmp(&b.order()));
    sorted
        .iter()
        .rev()
        .find_map(|e| parse_workflow(e))
        .map(|set| set.workflow)
        .unwrap_or_else(Workflow::default_v1)
}

impl Task {
    fn journal(&mut self, marker: &str, text: impl Into<String>, event: &Event) {
        self.journal.push(JournalEntry {
            marker: marker.into(),
            text: text.into(),
            at: event.at,
            actor: event.actor.clone(),
            event: event.id.clone(),
        });
    }

    fn clear_verdicts(&mut self) {
        for label in VERDICT_LABELS {
            self.labels.remove(label);
        }
    }
}

fn field(fields: &BTreeMap<String, String>, name: &str) -> String {
    fields.get(name).map(|v| v.trim().to_owned()).unwrap_or_default()
}

fn create(uuid: &str, event: &Event, wf: &Workflow) -> Option<Task> {
    let parsed = if event.v > EVENT_VERSION {
        None
    } else {
        serde_json::from_value::<Created>(event.payload.clone()).ok()
    };
    let Some(payload) = parsed else {
        return Some(Task {
            uuid: uuid.into(),
            project: event.project.clone(),
            title: "(unreadable task)".into(),
            prompt: String::new(),
            attachments: Vec::new(),
            stage: wf.first(Role::Backlog).map(|s| s.name.clone()).unwrap_or_default(),
            priority: 2,
            rank: String::new(),
            labels: BTreeSet::new(),
            pinned: None,
            assignee: None,
            reviewer: None,
            resume_stage: String::new(),
            parent: String::new(),
            follows: BTreeSet::new(),
            related: BTreeSet::new(),
            journal: Vec::new(),
            flags: BTreeSet::new(),
            head: event.id.clone(),
            deleted: false,
            read_only: true,
            created_at: event.at,
            updated_at: event.at,
            created_by: event.actor.clone(),
            rejections: BTreeMap::new(),
        });
    };
    let mut task = Task {
        uuid: uuid.into(),
        project: event.project.clone(),
        title: payload.title,
        prompt: payload.prompt,
        attachments: payload.attachments,
        stage: payload.stage.clone(),
        priority: payload.priority.min(4),
        rank: if rank::valid(&payload.rank) { payload.rank } else { String::new() },
        labels: payload.labels,
        pinned: payload.pinned,
        assignee: None,
        reviewer: None,
        resume_stage: String::new(),
        parent: payload.parent,
        follows: payload.follows,
        related: payload.related,
        journal: Vec::new(),
        flags: BTreeSet::new(),
        head: event.id.clone(),
        deleted: false,
        read_only: false,
        created_at: event.at,
        updated_at: event.at,
        created_by: event.actor.clone(),
        rejections: BTreeMap::new(),
    };
    match wf.allow(Action::Create, None, &payload.stage, event.actor.kind, &payload.fields) {
        Ok(_) => {
            let marker = if event.actor.kind == ActorKind::Agent && !payload.fields.contains_key("decomposed_from") {
                "[filed-by-agent]"
            } else {
                "[created]"
            };
            let text = payload.fields.get("breaks").cloned().unwrap_or_default();
            task.journal(marker, text, event);
        }
        Err(refusal) => {
            task.stage = wf.first(Role::Backlog).map(|s| s.name.clone()).unwrap_or_default();
            task.journal("[rejected]", refusal.message.clone(), event);
            task.rejections.insert(event.id.clone(), refusal);
        }
    }
    Some(task)
}

/// Every refusal comes before the first change, so a rejected event leaves only its `[rejected]` entry.
fn transition(task: &mut Task, event: &Event, wf: &Workflow) -> Result<(), Refusal> {
    let p: Transition = serde_json::from_value(event.payload.clone())
        .map_err(|_| Refusal::new("invalid_event", INVALID_EVENT))?;
    if event.base != task.head {
        return Err(Refusal::new("stale", "the task changed since this action was prepared"));
    }
    let actor = event.actor.kind;
    let from = task.stage.clone();
    let from_role = wf.role(&from).unwrap_or(Role::Backlog);
    if matches!(from_role, Role::Done | Role::Parked) && actor != ActorKind::Human {
        return Err(Refusal::new("task_closed", "this task is done or parked; only a person can change it"));
    }
    if p.action == Action::Answer && task.resume_stage.is_empty() {
        return Err(Refusal::new("no_question", "this task is not waiting for an answer"));
    }
    let target = if p.action == Action::Answer { RESUME.to_owned() } else { p.to.clone() };
    let rule = wf.allow(p.action, Some(&from), &target, actor, &p.fields)?;
    let own = rule.own;
    let to = if p.action == Action::Answer { task.resume_stage.clone() } else { p.to.clone() };
    let to_role = wf
        .role(&to)
        .ok_or_else(|| Refusal::new("unknown_stage", format!("unknown stage {to}")))?;
    if to_role == Role::Done && actor != ActorKind::Human {
        return Err(Refusal::new("done_is_human_only", "only a person can move a task to Done"));
    }
    if own && event.actor.session_ref() != task.assignee {
        return Err(Refusal::new("not_assigned", "this task is not assigned to your session"));
    }
    let text = |name: &str| field(&p.fields, name);
    match p.action {
        Action::Create => return Err(Refusal::new("invalid_event", "tasks are created by task_created")),
        Action::Claim => {
            if task.labels.contains("epic") {
                return Err(Refusal::new("epic", "an epic is a container and is never sent to a session"));
            }
            let session = p
                .session
                .clone()
                .ok_or_else(|| Refusal::field("missing_field", "session", "missing", "choose a session"))?;
            task.assignee = Some(session.clone());
            task.clear_verdicts();
            task.journal("[claim]", session.name, event);
        }
        Action::Unclaim => {
            task.assignee = None;
            task.journal("[unclaim]", text("reason"), event);
        }
        Action::Advance => {
            if to_role == Role::Review && task.labels.contains("bug") && text("root_cause").is_empty() {
                return Err(Refusal::field(
                    "missing_field",
                    "root_cause",
                    "missing",
                    "a bug needs `root_cause` before review",
                ));
            }
            // Any re-entry clears verdicts, including the one into Review.
            task.clear_verdicts();
            if to_role == Role::Review {
                let mut body = format!("Worklog: {}\nEvidence: {}", text("worklog"), text("evidence"));
                if !text("root_cause").is_empty() {
                    body = format!("Root cause: {}\n{body}", text("root_cause"));
                }
                task.journal("[worklog]", body, event);
            } else {
                task.journal("[spec]", text("spec"), event);
            }
        }
        Action::CallHuman => {
            task.resume_stage = from.clone();
            task.journal("[needs-human]", text("question"), event);
        }
        Action::Answer => {
            if task.resume_stage.is_empty() {
                return Err(Refusal::new("no_question", "this task is not waiting for an answer"));
            }
            task.resume_stage.clear();
            task.journal("[answer]", text("answer"), event);
        }
        Action::Return => {
            task.assignee = None;
            task.labels.insert("blocked".into());
            task.clear_verdicts();
            task.journal("[blocked]", text("reason"), event);
        }
        Action::Decompose => {
            if p.children.len() < 2 {
                return Err(Refusal::new("too_few_subtasks", "decompose into at least two subtasks"));
            }
            task.labels.insert("epic".into());
            task.assignee = None;
            task.clear_verdicts();
            task.journal("[decompose]", p.children.join(" "), event);
        }
        Action::Approve | Action::NeedsWork => {
            if task.labels.contains("epic") {
                return Err(Refusal::new("epic", "an epic is a container and is never reviewed"));
            }
            if actor != ActorKind::Human {
                let me = event.actor.session_ref();
                if me.is_none() || me != task.reviewer || me == task.assignee {
                    return Err(Refusal::new("not_reviewer", "only the session this task was sent to for review can review it"));
                }
            }
            task.clear_verdicts();
            let (label, verdict) = if p.action == Action::Approve {
                ("reviewed", "APPROVE")
            } else {
                ("review-failed", "NEEDS WORK")
            };
            task.labels.insert(label.into());
            task.journal("[review]", format!("{verdict}\n{}", text("report")), event);
        }
        Action::Done => task.journal("[done]", "", event),
        Action::Reopen => {
            task.clear_verdicts();
            task.journal("[moved]", format!("{from} -> {to}"), event);
        }
        Action::Move => task.journal("[moved]", format!("{from} -> {to}"), event),
    }
    // `orphaned` describes the assignee, so it goes when the assignee changes.
    // Changed within version 1 before the first release; see the spec's
    // Compatibility section.
    if matches!(p.action, Action::Claim | Action::Unclaim | Action::Return | Action::Decompose) {
        task.flags.remove("orphaned");
    }
    if p.action != Action::Answer && wf.role(&from) == Some(Role::NeedsHuman) {
        task.resume_stage.clear();
    }
    task.stage = to;
    task.head = event.id.clone();
    if rank::valid(&p.rank) {
        task.rank = p.rank;
    }
    Ok(())
}

fn rename(slot: &mut Option<SessionRef>, machine: &str, old: &str, new: &str) {
    if let Some(session) = slot {
        if session.machine == machine && session.name == old {
            session.name = new.into();
        }
    }
}

fn remap(stage: &str, old: &Workflow, new: &Workflow, moves: &BTreeMap<String, String>) -> String {
    if new.stage(stage).is_some() {
        return stage.into();
    }
    if let Some(target) = moves.get(stage).filter(|t| new.stage(t).is_some()) {
        return target.clone();
    }
    old.role(stage)
        .and_then(|role| new.first(role))
        .or_else(|| new.first(Role::Backlog))
        .map(|s| s.name.clone())
        .unwrap_or_default()
}

/// Non-transition events are gated here so no write path can bypass the rules.
fn gate(task: &Task, event: &Event, wf: &Workflow) -> Result<(), Refusal> {
    let actor = event.actor.kind;
    match event.kind.as_str() {
        "field_set" | "rank_set" | "label_set" | "relation_set" | "pin_set" | "reviewer_set" | "task_deleted"
            if actor != ActorKind::Human =>
        {
            Err(Refusal::new("human_only", "only a person can change this"))
        }
        "flag_set" if !matches!(actor, ActorKind::Human | ActorKind::Autopilot) => {
            Err(Refusal::new("human_only", "only a person or the autopilot can change flags"))
        }
        "journal" | "attachment_added"
            if actor != ActorKind::Human && matches!(wf.role(&task.stage), Some(Role::Done | Role::Parked)) =>
        {
            Err(Refusal::new("task_closed", "this task is done or parked; only a person can change it"))
        }
        _ => Ok(()),
    }
}

fn apply(task: &mut Task, event: &Event, wf: &mut Workflow) {
    if event.v > EVENT_VERSION {
        task.read_only = true;
        return;
    }
    let p = &event.payload;
    if event.task.is_empty() {
        match event.kind.as_str() {
            "workflow_set" if event.project == task.project => {
                if let Some(set) = parse_workflow(event) {
                    task.stage = remap(&task.stage, wf, &set.workflow, &set.moves);
                    if !task.resume_stage.is_empty() {
                        task.resume_stage = remap(&task.resume_stage, wf, &set.workflow, &set.moves);
                    }
                    *wf = set.workflow;
                }
            }
            // Only the session's host renames it; the actor names the machine that wrote the event.
            "session_renamed" if p.get("machine").and_then(Value::as_str) == Some(event.actor.machine.as_str()) => {
                let s = |k: &str| p.get(k).and_then(Value::as_str).unwrap_or_default().to_owned();
                let (machine, old, new) = (s("machine"), s("old"), s("new"));
                rename(&mut task.pinned, &machine, &old, &new);
                rename(&mut task.assignee, &machine, &old, &new);
                rename(&mut task.reviewer, &machine, &old, &new);
            }
            _ => {}
        }
        return;
    }
    task.updated_at = task.updated_at.max(event.at);
    if let Err(refusal) = gate(task, event, wf) {
        task.journal("[rejected]", refusal.message.clone(), event);
        task.rejections.insert(event.id.clone(), refusal);
        return;
    }
    let text = |k: &str| p.get(k).and_then(Value::as_str).unwrap_or_default().to_owned();
    // None: malformed (ignored); Some(None): explicit null (clears).
    let session = |k: &str| match p.get(k) {
        Some(Value::Null) => Some(None),
        Some(v) => serde_json::from_value::<SessionRef>(v.clone()).ok().map(Some),
        None => None,
    };
    match event.kind.as_str() {
        "task_created" => {}
        "field_set" => {
            let value = p.get("value").cloned().unwrap_or(Value::Null);
            match text("field").as_str() {
                "title" => {
                    if let Some(v) = value.as_str() {
                        task.title = v.into();
                    }
                }
                "prompt" => {
                    if let Some(v) = value.as_str() {
                        task.prompt = v.into();
                    }
                }
                "priority" => {
                    if let Some(v) = value.as_u64() {
                        task.priority = v.min(4) as u8;
                    }
                }
                "attachments" => {
                    if let Ok(v) = serde_json::from_value(value) {
                        task.attachments = v;
                    }
                }
                _ => task.read_only = true,
            }
            task.journal("[edited]", text("field"), event);
        }
        "rank_set" => {
            if let Some(rank) = p.get("rank").and_then(Value::as_str).filter(|r| rank::valid(r)) {
                task.rank = rank.into();
            }
        }
        "label_set" => {
            let label = text("label");
            if p.get("present").and_then(Value::as_bool).unwrap_or(false) {
                task.labels.insert(label);
            } else {
                task.labels.remove(&label);
            }
        }
        "relation_set" => {
            let target = text("target");
            let present = p.get("present").and_then(Value::as_bool).unwrap_or(false);
            match text("relation").as_str() {
                "parent" => {
                    if present {
                        task.parent = target;
                    } else if task.parent == target {
                        task.parent.clear();
                    }
                }
                "follows" => {
                    if present {
                        task.follows.insert(target);
                    } else {
                        task.follows.remove(&target);
                    }
                }
                "related" => {
                    if present {
                        task.related.insert(target);
                    } else {
                        task.related.remove(&target);
                    }
                }
                _ => task.read_only = true,
            }
        }
        "pin_set" => {
            if let Some(slot) = session("session") {
                task.pinned = slot;
            }
        }
        "reviewer_set" => {
            if let Some(slot) = session("session") {
                task.reviewer = slot;
            }
        }
        "flag_set" => {
            let flag = text("flag");
            if p.get("value").and_then(Value::as_bool).unwrap_or(false) {
                task.flags.insert(flag);
            } else {
                task.flags.remove(&flag);
            }
        }
        "attachment_added" => {
            if let Ok(attachment) = serde_json::from_value::<Attachment>(p.get("attachment").cloned().unwrap_or(Value::Null)) {
                task.attachments.push(attachment);
            }
        }
        "journal" => {
            let mut entry_text = text("text");
            if let Some(transition) = p.get("transition").and_then(Value::as_str) {
                if !transition.is_empty() {
                    entry_text = format!("{entry_text}\ntransition: {transition}");
                }
            }
            task.journal(&text("marker"), entry_text, event);
        }
        "task_deleted" => task.deleted = true,
        "transition" => {
            if let Err(refusal) = transition(task, event, wf) {
                task.journal("[rejected]", refusal.message.clone(), event);
                task.rejections.insert(event.id.clone(), refusal);
            }
        }
        _ => task.read_only = true,
    }
}

/// Folds one task from its events plus shared events, in any order.
pub fn fold(uuid: &str, events: &[&Event]) -> Option<Task> {
    let mut sorted: Vec<&Event> = events
        .iter()
        .copied()
        .filter(|e| e.task == uuid || (e.task.is_empty() && matches!(e.kind.as_str(), "workflow_set" | "session_renamed")))
        .collect();
    sorted.sort_by(|a, b| a.order().cmp(&b.order()));
    let mut seen = BTreeSet::new();
    sorted.retain(|e| seen.insert(e.id.clone()));
    let created_at = sorted.iter().position(|e| e.task == uuid && e.kind == "task_created")?;
    let created = sorted[created_at];
    let project = created.project.clone();
    let earlier: Vec<&Event> = sorted[..created_at].to_vec();
    let mut wf = project_workflow(&earlier, &project);
    let mut task = create(uuid, created, &wf)?;
    // Events of this task never precede its creation, and the shared events
    // that shape it must be readable, as after the creation.
    let unreadable = |e: &Event| e.v > EVENT_VERSION && (e.kind == "session_renamed" || e.project == project);
    if earlier.iter().any(|e| e.task == uuid || unreadable(e)) {
        task.read_only = true;
    }
    for event in &sorted[created_at + 1..] {
        if event.task.is_empty() && event.kind == "workflow_set" && event.project != project {
            continue;
        }
        apply(&mut task, event, &mut wf);
    }
    // A prompt delivered on a claim that lost the race cannot be recalled.
    let sent_on_rejected: Vec<(usize, String, &Event)> = sorted
        .iter()
        .enumerate()
        .filter(|(_, e)| e.task == uuid && e.kind == "journal" && !task.rejections.contains_key(&e.id))
        .filter(|(_, e)| matches!(e.payload.get("marker").and_then(Value::as_str), Some("[sent]") | Some("[sent?]")))
        .filter_map(|(i, e)| {
            let claim = e.payload.get("transition").and_then(Value::as_str)?;
            task.rejections.contains_key(claim).then(|| (i, claim.to_owned(), *e))
        })
        .collect();
    for (index, claim, event) in sent_on_rejected {
        let cleared = sorted[index + 1..].iter().any(|e| {
            e.task == uuid
                && e.kind == "flag_set"
                && e.v <= EVENT_VERSION
                && e.actor.kind == ActorKind::Human
                && e.payload.get("flag").and_then(Value::as_str) == Some("conflict")
                && e.payload.get("value").and_then(Value::as_bool) == Some(false)
        });
        if !cleared {
            task.flags.insert("conflict".into());
        }
        let position = task.journal.iter().position(|j| j.event == event.id && j.marker != "[conflict]");
        task.journal("[conflict]", format!("a prompt was sent on claim {claim}, which lost to another claim"), event);
        if let Some(position) = position {
            let entry = task.journal.pop().expect("just pushed");
            task.journal.insert(position + 1, entry);
        }
    }
    Some(task)
}

/// Every task found in `events`, keyed by UUID; deleted tasks are included.
pub fn fold_many(events: &[Event]) -> BTreeMap<String, Task> {
    let shared: Vec<&Event> = events.iter().filter(|e| e.task.is_empty()).collect();
    let mut by_task: BTreeMap<&str, Vec<&Event>> = BTreeMap::new();
    for event in events.iter().filter(|e| !e.task.is_empty()) {
        by_task.entry(event.task.as_str()).or_default().push(event);
    }
    by_task
        .into_iter()
        .filter_map(|(uuid, mut own)| {
            own.extend(shared.iter().copied());
            fold(uuid, &own).map(|task| (uuid.to_owned(), task))
        })
        .collect()
}
#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    const A: &str = "11111111-1111-4111-8111-111111111111";
    const B: &str = "22222222-2222-4222-8222-222222222222";
    const T: &str = "33333333-3333-4333-8333-333333333333";

    fn ev(origin: &str, seq: u64, lamport: u64, actor: Actor, kind: &str, base: &str, payload: Value) -> Event {
        Event {
            id: format!("{origin}-{seq}"), origin: origin.into(), seq, lamport, at: lamport, v: 1, actor,
            task: T.into(), project: "p".into(), kind: kind.into(), base: base.into(),
            request_id: String::new(), payload,
        }
    }
    fn human() -> Actor { Actor::human(A) }
    fn agent(name: &str) -> Actor { Actor::agent(A, name) }
    fn session(name: &str) -> Value { json!({"machine": A, "name": name}) }
    fn created(stage: &str) -> Event {
        ev(A, 1, 1, human(), "task_created", "", json!({"title": "Title", "prompt": "Do it", "stage": stage}))
    }
    fn transition(origin: &str, seq: u64, lamport: u64, actor: Actor, base: &str, action: &str, to: &str, fields: Value, session: Value) -> Event {
        ev(origin, seq, lamport, actor, "transition", base, json!({"action": action, "to": to, "fields": fields, "session": session}))
    }
    fn run(events: &[Event]) -> Task {
        let refs: Vec<&Event> = events.iter().collect();
        fold(T, &refs).unwrap()
    }

    #[test]
    fn creation_sets_the_initial_state() {
        let task = run(&[created("Queue")]);
        assert_eq!((task.stage.as_str(), task.title.as_str(), task.prompt.as_str(), task.priority), ("Queue", "Title", "Do it", 2));
        assert_eq!(task.journal[0].marker, "[created]");
        assert_eq!(task.head, format!("{A}-1"));
    }

    #[test]
    fn claim_and_advance_follow_the_gates() {
        let c = created("Queue");
        let claim = transition(A, 2, 2, human(), &c.id, "claim", "Design", json!({}), session("w1"));
        let no_spec = transition(A, 3, 3, agent("w1"), &claim.id, "advance", "Build", json!({}), Value::Null);
        let spec = transition(A, 4, 4, agent("w1"), &claim.id, "advance", "Build", json!({"spec": "plan"}), Value::Null);
        let task = run(&[c, claim, no_spec.clone(), spec]);
        assert_eq!(task.stage, "Build");
        assert_eq!(task.assignee.as_ref().unwrap().name, "w1");
        assert_eq!(task.rejections[&no_spec.id].field, "spec");
        assert!(task.journal.iter().any(|j| j.marker == "[spec]" && j.text == "plan"));
        assert!(task.journal.iter().any(|j| j.marker == "[rejected]"));
    }

    #[test]
    fn other_sessions_cannot_act_on_the_task() {
        let c = created("Queue");
        let claim = transition(A, 2, 2, human(), &c.id, "claim", "Design", json!({}), session("w1"));
        let intruder = transition(A, 3, 3, agent("w2"), &claim.id, "advance", "Build", json!({"spec": "x"}), Value::Null);
        let task = run(&[c, claim, intruder.clone()]);
        assert_eq!(task.stage, "Design");
        assert_eq!(task.rejections[&intruder.id].code, "not_assigned");
    }

    #[test]
    fn agents_never_reach_done_by_any_action() {
        let c = created("Queue");
        let claim = transition(A, 2, 2, human(), &c.id, "claim", "Design", json!({}), session("w1"));
        let spec = transition(A, 3, 3, agent("w1"), &claim.id, "advance", "Build", json!({"spec": "s"}), Value::Null);
        let review = transition(A, 4, 4, agent("w1"), &spec.id, "advance", "Review", json!({"worklog": "w", "evidence": "e"}), Value::Null);
        assert_eq!(run(&[c.clone(), claim.clone(), spec.clone(), review.clone()]).stage, "Review");
        for action in Action::ALL {
            let name = serde_json::to_value(action).unwrap();
            let attempt = transition(A, 5, 5, agent("w1"), &review.id, name.as_str().unwrap(), "Done", json!({"report": "r"}), Value::Null);
            let task = run(&[c.clone(), claim.clone(), spec.clone(), review.clone(), attempt]);
            assert_eq!(task.stage, "Review", "{action:?}");
        }
    }

    #[test]
    fn concurrent_claims_resolve_identically_in_any_delivery_order() {
        let c = created("Queue");
        let first = transition(A, 2, 2, human(), &c.id, "claim", "Design", json!({}), session("w1"));
        let second = transition(B, 1, 2, Actor::human(B), &c.id, "claim", "Design", json!({}), json!({"machine": B, "name": "w9"}));
        let label = ev(B, 2, 3, Actor::human(B), "label_set", "", json!({"label": "ui", "present": true}));
        let mut wf_set = ev(B, 3, 4, Actor::human(B), "workflow_set", "", json!({"workflow": Workflow::default_v1(), "moves": {}}));
        wf_set.task = String::new();
        let mut renamed = ev(A, 2, 5, human(), "session_renamed", "", json!({"machine": A, "old": "zz", "new": "yy"}));
        renamed.task = String::new();
        renamed.project = String::new();
        renamed.id = "rename-1".into();
        let mut events = vec![c, first, second.clone(), label, wf_set, renamed];
        let expected = serde_json::to_value(run(&events)).unwrap();
        assert_eq!(expected["assignee"]["name"], "w1");
        let mut seed = 7u64;
        for _ in 0..200 {
            for i in (1..events.len()).rev() {
                seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                events.swap(i, (seed % (i as u64 + 1)) as usize);
            }
            assert_eq!(serde_json::to_value(run(&events)).unwrap(), expected);
        }
        assert_eq!(run(&events).rejections[&second.id].code, "stale");
    }

    #[test]
    fn a_sent_prompt_on_a_rejected_claim_is_a_conflict() {
        let c = created("Queue");
        let first = transition(A, 2, 2, human(), &c.id, "claim", "Design", json!({}), session("w1"));
        let second = transition(B, 1, 2, Actor::human(B), &c.id, "claim", "Design", json!({}), json!({"machine": B, "name": "w9"}));
        let sent = ev(B, 2, 3, Actor::human(B), "journal", "", json!({"marker": "[sent]", "text": "w9", "transition": second.id}));
        let task = run(&[c, first, second, sent]);
        assert!(task.flags.contains("conflict"));
        assert!(task.journal.iter().any(|j| j.marker == "[conflict]"));
    }

    #[test]
    fn your_call_returns_to_the_stage_it_left() {
        let c = created("Queue");
        let claim = transition(A, 2, 2, human(), &c.id, "claim", "Design", json!({}), session("w1"));
        let spec = transition(A, 3, 3, agent("w1"), &claim.id, "advance", "Build", json!({"spec": "s"}), Value::Null);
        let ask = transition(A, 4, 4, agent("w1"), &spec.id, "call_human", "Your Call", json!({"question": "Which API?"}), Value::Null);
        let answer = transition(A, 5, 5, human(), &ask.id, "answer", "", json!({"answer": "REST"}), Value::Null);
        let task = run(&[c, claim, spec, ask, answer]);
        assert_eq!(task.stage, "Build");
        assert_eq!(task.assignee.as_ref().unwrap().name, "w1");
    }

    #[test]
    fn bugs_need_a_root_cause_and_reviews_need_independence() {
        let c = created("Queue");
        let bug = ev(A, 2, 2, human(), "label_set", "", json!({"label": "bug", "present": true}));
        let claim = transition(A, 3, 3, human(), &c.id, "claim", "Design", json!({}), session("w1"));
        let spec = transition(A, 4, 4, agent("w1"), &claim.id, "advance", "Build", json!({"spec": "s"}), Value::Null);
        let no_cause = transition(A, 5, 5, agent("w1"), &spec.id, "advance", "Review", json!({"worklog": "w", "evidence": "e"}), Value::Null);
        let done = transition(A, 6, 6, agent("w1"), &spec.id, "advance", "Review", json!({"worklog": "w", "evidence": "e", "root_cause": "r"}), Value::Null);
        let self_review = transition(A, 7, 7, agent("w1"), &done.id, "approve", "Review", json!({"report": "ok"}), Value::Null);
        let reviewer = ev(A, 8, 8, human(), "reviewer_set", "", json!({"session": session("w2")}));
        let review = transition(A, 9, 9, agent("w2"), &done.id, "approve", "Review", json!({"report": "ok"}), Value::Null);
        let task = run(&[c, bug, claim, spec, no_cause.clone(), done, self_review.clone(), reviewer, review]);
        assert_eq!(task.rejections[&no_cause.id].field, "root_cause");
        assert_eq!(task.rejections[&self_review.id].code, "not_reviewer");
        assert!(task.labels.contains("reviewed"));
    }

    #[test]
    fn a_rejected_advance_keeps_the_verdict() {
        let c = created("Queue");
        let bug = ev(A, 2, 2, human(), "label_set", "", json!({"label": "bug", "present": true}));
        let claim = transition(A, 3, 3, human(), &c.id, "claim", "Design", json!({}), session("w1"));
        let spec = transition(A, 4, 4, agent("w1"), &claim.id, "advance", "Build", json!({"spec": "s"}), Value::Null);
        let review = transition(A, 5, 5, agent("w1"), &spec.id, "advance", "Review", json!({"worklog": "w", "evidence": "e", "root_cause": "r"}), Value::Null);
        let reviewer = ev(A, 6, 6, human(), "reviewer_set", "", json!({"session": session("w2")}));
        let needs_work = transition(A, 7, 7, agent("w2"), &review.id, "needs_work", "Build", json!({"report": "fix"}), Value::Null);
        let no_cause = transition(A, 8, 8, agent("w1"), &needs_work.id, "advance", "Review", json!({"worklog": "w", "evidence": "e"}), Value::Null);
        let events = [c, bug, claim, spec, review, reviewer, needs_work.clone(), no_cause.clone()];
        let task = run(&events);
        assert_eq!(task.rejections[&no_cause.id].field, "root_cause");
        assert_eq!((task.stage.as_str(), task.head.as_str()), ("Build", needs_work.id.as_str()));
        assert!(task.labels.contains("review-failed"));
        let fixed = transition(A, 9, 9, agent("w1"), &needs_work.id, "advance", "Review", json!({"worklog": "w", "evidence": "e", "root_cause": "r"}), Value::Null);
        let mut events = events.to_vec();
        events.push(fixed);
        assert!(!run(&events).labels.contains("review-failed"));
    }

    #[test]
    fn events_before_the_creation_make_the_task_read_only() {
        let c = ev(A, 2, 5, human(), "task_created", "", json!({"title": "T", "prompt": "p", "stage": "Queue"}));
        let early = ev(B, 1, 3, Actor::human(B), "journal", "", json!({"marker": "[comment]", "text": "x"}));
        let task = run(&[c.clone(), early]);
        assert!(task.read_only);
        assert!(task.journal.iter().all(|j| j.marker != "[comment]"));
        let mut newer = ev(B, 1, 3, Actor::human(B), "workflow_set", "", json!({}));
        newer.task = String::new();
        newer.v = EVENT_VERSION + 1;
        assert!(run(&[c.clone(), newer.clone()]).read_only);
        let mut renamed = newer.clone();
        renamed.kind = "session_renamed".into();
        renamed.project = String::new();
        assert!(run(&[c.clone(), renamed]).read_only);
        let mut elsewhere = newer.clone();
        elsewhere.project = "other".into();
        assert!(!run(&[c.clone(), elsewhere]).read_only);
        let mut readable = newer;
        readable.v = EVENT_VERSION;
        assert!(!run(&[c, readable]).read_only);
    }

    #[test]
    fn workflow_change_remaps_removed_stage() {
        let c = created("Queue");
        let mut wf = Workflow::default_v1();
        wf.stages[1].name = "Ready".into();
        for rule in &mut wf.rules {
            for f in &mut rule.from { if f == "Queue" { *f = "Ready".into(); } }
            if rule.to == "Queue" { rule.to = "Ready".into(); }
        }
        let mut set = ev(A, 2, 2, human(), "workflow_set", "", json!({"workflow": wf, "moves": {}}));
        set.task = String::new();
        let task = run(&[c, set]);
        assert_eq!(task.stage, "Ready");
    }

    #[test]
    fn renames_follow_sessions_and_newer_events_make_tasks_read_only() {
        let c = created("Queue");
        let pin = ev(A, 2, 2, human(), "pin_set", "", json!({"session": session("old")}));
        let mut rename = ev(A, 3, 3, human(), "session_renamed", "", json!({"machine": A, "old": "old", "new": "new"}));
        rename.task = String::new();
        rename.project = String::new();
        let mut future = ev(A, 4, 4, human(), "field_set", "", json!({"field": "title", "value": "x"}));
        future.v = 2;
        let task = run(&[c, pin, rename, future]);
        assert_eq!(task.pinned.unwrap().name, "new");
        assert!(task.read_only);
        assert_eq!(task.title, "Title");
    }

    #[test]
    fn renames_apply_only_when_written_by_the_sessions_host() {
        let c = created("Queue");
        let pin = ev(A, 2, 2, human(), "pin_set", "", json!({"session": session("old")}));
        // Origins are stores, not machines: the actor names the machine that wrote it.
        let store = "44444444-4444-4444-8444-444444444444";
        let rename = |origin: &str, actor: Actor| {
            let mut event = ev(origin, 1, 3, actor, "session_renamed", "", json!({"machine": A, "old": "old", "new": "new"}));
            event.task = String::new();
            event.project = String::new();
            event
        };
        assert_eq!(run(&[c.clone(), pin.clone(), rename(store, human())]).pinned.unwrap().name, "new");
        assert_eq!(run(&[c.clone(), pin.clone(), rename(A, Actor::human(B))]).pinned.unwrap().name, "old");
        assert_eq!(run(&[c, pin, rename(store, Actor::human(B))]).pinned.unwrap().name, "old");
    }

    #[test]
    fn fold_many_finds_every_task() {
        let c = created("Queue");
        let mut other = created("Backlog");
        other.task = "44444444-4444-4444-8444-444444444444".into();
        other.id = "other".into();
        other.seq = 2;
        let tasks = fold_many(&[c, other]);
        assert_eq!(tasks.len(), 2);
    }

    fn claimed() -> (Event, Event) {
        let c = created("Queue");
        let claim = transition(A, 2, 2, human(), &c.id, "claim", "Design", json!({}), session("w1"));
        (c, claim)
    }

    #[test]
    fn workflow_set_by_non_humans_is_ignored() {
        let c = created("Queue");
        let mut set = ev(A, 2, 2, agent("w1"), "workflow_set", "", json!({"workflow": Workflow::default_v1(), "moves": {"Queue": "Done"}}));
        set.task = String::new();
        let mut wf = Workflow::default_v1();
        wf.stages.retain(|s| s.name != "Queue");
        set.payload = json!({"workflow": wf, "moves": {"Queue": "Done"}});
        assert_eq!(run(&[c.clone(), set.clone()]).stage, "Queue");
        let refs: Vec<&Event> = vec![&set];
        assert_eq!(project_workflow(&refs, "p").stages.len(), Workflow::default_v1().stages.len());
    }

    #[test]
    fn unreadable_creations_still_yield_read_only_tasks() {
        let mut future = created("Queue");
        future.v = 2;
        let task = run(&[future]);
        assert!(task.read_only);
        assert_eq!(task.title, "(unreadable task)");
        let bad = ev(A, 1, 1, human(), "task_created", "", json!({"title": 5}));
        let task = run(&[bad.clone()]);
        assert!(task.read_only && task.prompt.is_empty());
        assert_eq!(task.head, bad.id);
        assert!(!task.stage.is_empty());
    }

    #[test]
    fn conflicts_can_be_cleared_and_sit_next_to_the_prompt() {
        let c = created("Queue");
        let first = transition(A, 2, 2, human(), &c.id, "claim", "Design", json!({}), session("w1"));
        let second = transition(B, 1, 2, Actor::human(B), &c.id, "claim", "Design", json!({}), json!({"machine": B, "name": "w9"}));
        let sent = ev(B, 2, 3, Actor::human(B), "journal", "", json!({"marker": "[sent]", "text": "w9", "transition": second.id}));
        let after = ev(B, 3, 4, Actor::human(B), "journal", "", json!({"marker": "[comment]", "text": "later"}));
        let task = run(&[c.clone(), first.clone(), second.clone(), sent.clone(), after.clone()]);
        let i = task.journal.iter().position(|j| j.event == sent.id).unwrap();
        assert_eq!(task.journal[i + 1].marker, "[conflict]");
        assert_eq!(task.journal[i + 2].marker, "[comment]");
        let clear = ev(B, 4, 5, Actor::human(B), "flag_set", "", json!({"flag": "conflict", "value": false}));
        let task = run(&[c.clone(), first.clone(), second.clone(), sent.clone(), clear]);
        assert!(!task.flags.contains("conflict"));
        let winner = ev(B, 5, 3, Actor::human(B), "journal", "", json!({"marker": "[sent]", "text": "w1", "transition": first.id}));
        let task = run(&[c, first, second, winner]);
        assert!(!task.flags.contains("conflict"));
        assert!(!task.journal.iter().any(|j| j.marker == "[conflict]"));
    }

    #[test]
    fn non_transition_events_are_gated() {
        let (c, claim) = claimed();
        let edit = ev(A, 3, 3, agent("w1"), "field_set", "", json!({"field": "title", "value": "x"}));
        let flag = ev(A, 4, 4, agent("w1"), "flag_set", "", json!({"flag": "f", "value": true}));
        let auto = ev(A, 5, 5, Actor { kind: ActorKind::Autopilot, ..human() }, "flag_set", "", json!({"flag": "g", "value": true}));
        let task = run(&[c.clone(), claim.clone(), edit.clone(), flag.clone(), auto]);
        assert_eq!(task.title, "Title");
        assert_eq!(task.rejections[&edit.id].code, "human_only");
        assert_eq!(task.rejections[&flag.id].code, "human_only");
        assert!(task.flags.contains("g") && !task.flags.contains("f"));
        let park = transition(A, 3, 3, human(), &claim.id, "move", "Icebox", json!({}), Value::Null);
        let note = ev(A, 4, 4, agent("w1"), "journal", "", json!({"marker": "[comment]", "text": "hi"}));
        let task = run(&[c, claim, park, note.clone()]);
        assert_eq!(task.rejections[&note.id].code, "task_closed");
    }

    #[test]
    fn rank_and_creation_extras() {
        let c = ev(A, 1, 1, human(), "task_created", "", json!({"title": "T", "prompt": "p", "stage": "Queue", "rank": "V", "related": ["x"]}));
        let good = ev(A, 2, 2, human(), "rank_set", "", json!({"rank": "W"}));
        let bad = ev(A, 3, 3, human(), "rank_set", "", json!({"rank": "a b"}));
        let task = run(&[c.clone(), good]);
        assert_eq!(task.rank, "W");
        assert!(task.related.contains("x"));
        assert_eq!(run(&[c, bad]).rank, "V");
    }

    #[test]
    fn created_ranks_are_validated_and_accepted_transitions_carry_ranks() {
        let bad = ev(A, 1, 1, human(), "task_created", "", json!({"title": "T", "prompt": "p", "stage": "Queue", "rank": "a b"}));
        assert_eq!(run(&[bad]).rank, "");
        let c = created("Queue");
        let claim = ev(A, 2, 2, human(), "transition", &c.id, json!({"action": "claim", "to": "Design", "session": session("w1"), "rank": "k"}));
        assert_eq!(run(&[c.clone(), claim.clone()]).rank, "k");
        let stale = ev(A, 3, 3, human(), "transition", &c.id, json!({"action": "move", "to": "Icebox", "rank": "m"}));
        let task = run(&[c.clone(), claim.clone(), stale.clone()]);
        assert_eq!((task.rank.as_str(), task.rejections[&stale.id].code.as_str()), ("k", "stale"));
        let junk = ev(A, 3, 3, human(), "transition", &claim.id, json!({"action": "move", "to": "Icebox", "rank": "no rank"}));
        let task = run(&[c, claim, junk]);
        assert_eq!((task.stage.as_str(), task.rank.as_str()), ("Icebox", "k"));
    }

    #[test]
    fn removing_a_parent_needs_the_current_parent() {
        let c = created("Queue");
        let set = ev(A, 2, 2, human(), "relation_set", "", json!({"relation": "parent", "target": "x", "present": true}));
        let other = ev(A, 3, 3, human(), "relation_set", "", json!({"relation": "parent", "target": "y", "present": false}));
        assert_eq!(run(&[c.clone(), set.clone(), other]).parent, "x");
        let clear = ev(A, 3, 3, human(), "relation_set", "", json!({"relation": "parent", "target": "x", "present": false}));
        assert_eq!(run(&[c, set, clear]).parent, "");
    }

    #[test]
    fn advancing_into_review_clears_earlier_verdicts() {
        let (c, claim) = claimed();
        let spec = transition(A, 3, 3, agent("w1"), &claim.id, "advance", "Build", json!({"spec": "s"}), Value::Null);
        let review = transition(A, 4, 4, agent("w1"), &spec.id, "advance", "Review", json!({"worklog": "w", "evidence": "e"}), Value::Null);
        let failed = transition(A, 5, 5, human(), &review.id, "needs_work", "Build", json!({"report": "r"}), Value::Null);
        let again = transition(A, 6, 6, agent("w1"), &failed.id, "advance", "Review", json!({"worklog": "w", "evidence": "e"}), Value::Null);
        let events = [c, claim, spec, review, failed];
        assert!(run(&events).labels.contains("review-failed"));
        let mut all = events.to_vec();
        all.push(again);
        let task = run(&all);
        assert_eq!(task.stage, "Review");
        assert!(!task.labels.contains("review-failed"));
    }

    #[test]
    fn answers_pins_renames_and_epics_are_strict() {
        let (c, claim) = claimed();
        let answer = transition(A, 3, 3, human(), &claim.id, "answer", "", json!({"answer": "x"}), Value::Null);
        assert_eq!(run(&[c.clone(), claim.clone(), answer.clone()]).rejections[&answer.id].code, "no_question");
        let pin = ev(A, 3, 3, human(), "pin_set", "", json!({"session": session("p1")}));
        let junk = ev(A, 4, 4, human(), "pin_set", "", json!({"session": 5}));
        assert_eq!(run(&[c.clone(), pin.clone(), junk]).pinned.unwrap().name, "p1");
        let clear = ev(A, 5, 5, human(), "pin_set", "", json!({"session": null}));
        assert!(run(&[c.clone(), pin.clone(), clear]).pinned.is_none());
        // Written on machine B for a session of machine A.
        let mut foreign = ev(B, 1, 6, Actor::human(B), "session_renamed", "", json!({"machine": A, "old": "p1", "new": "q"}));
        foreign.task = String::new();
        assert_eq!(run(&[c.clone(), pin, foreign]).pinned.unwrap().name, "p1");
        let spec = transition(A, 3, 3, agent("w1"), &claim.id, "advance", "Build", json!({"spec": "s"}), Value::Null);
        let review = transition(A, 4, 4, agent("w1"), &spec.id, "advance", "Review", json!({"worklog": "w", "evidence": "e"}), Value::Null);
        let epic = ev(A, 5, 5, human(), "label_set", "", json!({"label": "epic", "present": true}));
        let approve = transition(A, 6, 6, human(), &review.id, "approve", "Review", json!({"report": "r"}), Value::Null);
        let task = run(&[c, claim, spec, review, epic, approve.clone()]);
        assert_eq!(task.rejections[&approve.id].code, "epic");
    }

    #[test]
    fn a_change_of_assignee_clears_the_orphaned_flag() {
        let (c, claim) = claimed();
        let orphan = |seq: u64| ev(A, seq, seq, human(), "flag_set", "", json!({"flag": "orphaned", "value": true}));
        let flags = |events: &[Event]| run(events).flags.into_iter().collect::<Vec<_>>();
        assert_eq!(flags(&[c.clone(), claim.clone(), orphan(3)]), ["orphaned"]);
        // Moves keep the assignee and the flag; so does a refused unclaim.
        let park = transition(A, 4, 4, human(), &claim.id, "move", "Icebox", json!({}), Value::Null);
        assert_eq!(flags(&[c.clone(), claim.clone(), orphan(3), park]), ["orphaned"]);
        let stale = transition(A, 4, 4, human(), &c.id, "unclaim", "Queue", json!({}), Value::Null);
        assert_eq!(flags(&[c.clone(), claim.clone(), orphan(3), stale]), ["orphaned"]);
        let unclaim = transition(A, 4, 4, human(), &claim.id, "unclaim", "Queue", json!({}), Value::Null);
        let task = run(&[c.clone(), claim.clone(), orphan(3), unclaim.clone()]);
        assert_eq!((task.stage.as_str(), task.assignee, task.flags.len()), ("Queue", None, 0));
        // A flag that arrives after the unclaim is cleared by the next claim.
        let again = transition(A, 6, 6, human(), &unclaim.id, "claim", "Design", json!({}), session("w2"));
        let task = run(&[c.clone(), claim.clone(), unclaim.clone(), orphan(5), again]);
        assert_eq!((task.assignee.unwrap().name.as_str(), task.flags.len()), ("w2", 0));
        let back = transition(A, 4, 4, agent("w1"), &claim.id, "return", "Backlog", json!({"reason": "blocked"}), Value::Null);
        assert_eq!(flags(&[c, claim, orphan(3), back]), Vec::<String>::new());
    }

    #[test]
    fn leaving_your_call_clears_the_resume_stage_and_duplicates_collapse() {
        let (c, claim) = claimed();
        let spec = transition(A, 3, 3, agent("w1"), &claim.id, "advance", "Build", json!({"spec": "s"}), Value::Null);
        let ask = transition(A, 4, 4, agent("w1"), &spec.id, "call_human", "Your Call", json!({"question": "q"}), Value::Null);
        let ret = transition(A, 5, 5, human(), &ask.id, "move", "Icebox", json!({}), Value::Null);
        let task = run(&[c.clone(), claim.clone(), spec.clone(), ask.clone(), ret, ask.clone()]);
        assert_eq!(task.stage, "Icebox");
        assert!(task.resume_stage.is_empty());
        assert_eq!(task.journal.iter().filter(|j| j.marker == "[needs-human]").count(), 1);
    }
}
