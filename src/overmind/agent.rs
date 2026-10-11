//! Commands for agents in Zerus sessions. Each one is narrow and refuses moves
//! that would break the process; results carry a `note` with the next step.
use super::commands::{
    check_project, check_text, created_payload, load, next_rank, parse, prove, reference, refuse, resolve,
    store_attachments, transition, view, workflow, write, Ctx, NewAttachment, MAX_FIELD,
};
use super::fold::{fold_many, Task};
use super::model::*;
use super::store::{Filter, Tx};
use super::workflow::{Action, Evidence, Role};
use anyhow::Result;
use serde::Deserialize;
use serde_json::{json, Value};
use std::collections::{BTreeMap, BTreeSet};
use std::path::Path;
use std::process::Command;

fn agent_only(actor: &Actor) -> Result<SessionRef> {
    match (actor.kind, actor.session_ref()) {
        (ActorKind::Agent, Some(me)) => Ok(me),
        _ => refuse("agent_only", "this command is for agents working in a Zerus session"),
    }
}

fn result(tx: &Tx, uuid: &str, note: &str) -> Result<Value> {
    let task = load(tx, uuid)?;
    Ok(json!({"ok": true, "task": view(&task, &reference(tx, uuid)?, true), "note": note}))
}

fn fields(pairs: &[(&str, &Option<String>)]) -> BTreeMap<String, String> {
    pairs
        .iter()
        .filter_map(|(k, v)| v.as_ref().map(|v| (k.to_string(), v.clone())))
        .collect()
}

pub fn mine(ctx: &mut Ctx) -> Result<Value> {
    let me = agent_only(&ctx.actor)?;
    let events = ctx.store.load(&Filter::All)?;
    let names = super::refs::refs(&ctx.store.task_ids()?);
    let tasks: Vec<Value> = fold_many(&events)
        .into_values()
        .filter(|t| !t.deleted && (t.assignee.as_ref() == Some(&me) || t.reviewer.as_ref() == Some(&me)))
        .map(|t| {
            let mut v = view(&t, names.get(&t.uuid).map(String::as_str).unwrap_or_default(), false);
            v["role"] = json!(if t.reviewer.as_ref() == Some(&me) { "reviewer" } else { "assignee" });
            v
        })
        .collect();
    Ok(json!({"ok": true, "tasks": tasks,
        "note": "Work on one task at a time. Read it with `show`, then advance it; use call-human (MCP: call_human) when a decision is not yours to make. You can never move a task to Done; a person signs it off."}))
}

fn verify_evidence(cwd: &Path, evidence: &str) -> Result<()> {
    let inside = Command::new("git")
        .arg("-C").arg(cwd)
        .args(["rev-parse", "--is-inside-work-tree"])
        .output()
        .map(|o| o.status.success() && String::from_utf8_lossy(&o.stdout).trim() == "true")
        .unwrap_or(false);
    if !inside {
        return Ok(());
    }
    let sha = evidence.trim();
    let hex = (7..=64).contains(&sha.len()) && sha.bytes().all(|b| b.is_ascii_hexdigit());
    if !hex {
        return Err(Refusal::field("invalid_field", "evidence", "invalid", "in a Git repository, evidence is the commit SHA of the finished work").into());
    }
    let exists = Command::new("git")
        .arg("-C").arg(cwd)
        .args(["cat-file", "-e", &format!("{sha}^{{commit}}")])
        .status()
        .map(|s| s.success())
        .unwrap_or(false);
    if !exists {
        return Err(Refusal::field("invalid_field", "evidence", "invalid", format!("commit {sha} does not exist in this repository")).into());
    }
    Ok(())
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct AdvanceRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    to: String,
    #[serde(default)]
    spec: Option<String>,
    #[serde(default)]
    worklog: Option<String>,
    #[serde(default)]
    evidence: Option<String>,
    #[serde(default)]
    root_cause: Option<String>,
}

pub fn advance(ctx: &mut Ctx, request: Value) -> Result<Value> {
    agent_only(&ctx.actor)?;
    let req: AdvanceRequest = parse(&request)?;
    let cwd = ctx.cwd.clone();
    write(ctx, &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load(tx, &uuid)?;
        let wf = workflow(tx, &task.project)?;
        let to_role = wf.role(&req.to).ok_or_else(|| Refusal::new("unknown_stage", format!("unknown stage {}", req.to)))?;
        let fields = fields(&[("spec", &req.spec), ("worklog", &req.worklog), ("evidence", &req.evidence), ("root_cause", &req.root_cause)]);
        wf.allow(Action::Advance, Some(&task.stage), &req.to, actor.kind, &fields)?;
        if to_role == Role::Review {
            for predecessor in &task.follows {
                let before = match load(tx, predecessor) {
                    Ok(before) => before,
                    Err(error) => match error.downcast_ref::<Refusal>() {
                        Some(r) if r.code == "unknown_task" => continue,
                        _ => return Err(error),
                    },
                };
                let role = workflow(tx, &before.project)?.role(&before.stage);
                if !matches!(role, Some(Role::Review) | Some(Role::Done)) {
                    return refuse("predecessor_open", format!("{} must reach review first", reference(tx, predecessor)?));
                }
            }
            if wf.evidence == Evidence::GitShaOrText {
                if let (Some(cwd), Some(evidence)) = (&cwd, &req.evidence) {
                    verify_evidence(cwd, evidence)?;
                }
            }
        }
        transition(tx, actor, &task, Action::Advance, &req.to, fields, None, vec![], &req.request_id)?;
        let note = if to_role == Role::Review {
            "Done from your side: the task waits for review. Take your next task."
        } else {
            "Continue the task; when it is finished, advance it to Review with a worklog and evidence (the commit SHA in a Git repository)."
        };
        result(tx, &uuid, note)
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct QuestionRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    question: String,
}

pub fn call_human(ctx: &mut Ctx, request: Value) -> Result<Value> {
    agent_only(&ctx.actor)?;
    let req: QuestionRequest = parse(&request)?;
    write(ctx, &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load(tx, &uuid)?;
        let to = workflow(tx, &task.project)?.first(Role::NeedsHuman).map(|s| s.name.clone()).unwrap_or_default();
        let fields = BTreeMap::from([("question".to_string(), req.question.clone())]);
        transition(tx, actor, &task, Action::CallHuman, &to, fields, None, vec![], &req.request_id)?;
        result(tx, &uuid, "The task waits for a person's answer; stop working on it until the answer arrives.")
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ReasonRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    reason: String,
}

pub fn return_task(ctx: &mut Ctx, request: Value) -> Result<Value> {
    agent_only(&ctx.actor)?;
    let req: ReasonRequest = parse(&request)?;
    write(ctx, &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load(tx, &uuid)?;
        let to = workflow(tx, &task.project)?.first(Role::Backlog).map(|s| s.name.clone()).unwrap_or_default();
        let fields = BTreeMap::from([("reason".to_string(), req.reason.clone())]);
        transition(tx, actor, &task, Action::Return, &to, fields, None, vec![], &req.request_id)?;
        result(tx, &uuid, "The task went back to Backlog as blocked; a person will triage it.")
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Subtask {
    #[serde(default)]
    title: String,
    prompt: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct DecomposeRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    subtasks: Vec<Subtask>,
    #[serde(default)]
    ordered: bool,
}

pub fn decompose(ctx: &mut Ctx, request: Value) -> Result<Value> {
    let me = agent_only(&ctx.actor)?;
    let req: DecomposeRequest = parse(&request)?;
    if req.subtasks.len() < 2 || req.subtasks.len() > 20 {
        return refuse("too_few_subtasks", "decompose into 2 to 20 subtasks");
    }
    write(ctx, &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load(tx, &uuid)?;
        if task.assignee.as_ref() != Some(&me) {
            return refuse("not_assigned", "this task is not assigned to your session");
        }
        let ready = workflow(tx, &task.project)?.first(Role::Ready).map(|s| s.name.clone()).unwrap_or_default();
        let mut children: Vec<String> = Vec::new();
        for sub in &req.subtasks {
            let follows: BTreeSet<String> = match (req.ordered, children.last()) {
                (true, Some(previous)) => BTreeSet::from([previous.clone()]),
                _ => BTreeSet::new(),
            };
            let fields = BTreeMap::from([("decomposed_from".to_string(), uuid.clone())]);
            let rank = next_rank(tx, &task.project, &ready)?;
            let payload = created_payload(&sub.title, &sub.prompt, &ready, Some(task.priority), &[], None, &uuid, follows, BTreeSet::new(), vec![], fields, rank)?;
            let child = uuid::Uuid::new_v4().to_string();
            prove(tx, Draft { actor: actor.clone(), task: child.clone(), project: task.project.clone(), kind: "task_created".into(), base: String::new(), request_id: String::new(), payload })?;
            children.push(child);
        }
        let backlog = workflow(tx, &task.project)?.first(Role::Backlog).map(|s| s.name.clone()).unwrap_or_default();
        transition(tx, actor, &task, Action::Decompose, &backlog, BTreeMap::new(), None, children.clone(), &req.request_id)?;
        let views = children
            .iter()
            .map(|c| Ok(view(&load(tx, c)?, &reference(tx, c)?, false)))
            .collect::<Result<Vec<_>>>()?;
        let mut out = result(tx, &uuid, "The subtasks are in Queue; the original task is now an epic container.")?;
        out["children"] = json!(views);
        Ok(out)
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct FileRequest {
    #[serde(default)]
    request_id: String,
    prompt: String,
    #[serde(default)]
    breaks: String,
    #[serde(default)]
    title: String,
    #[serde(default)]
    related: String,
    #[serde(default)]
    project: Option<String>,
}

fn my_tasks(tx: &Tx, me: &SessionRef) -> Result<Vec<Task>> {
    let events = tx.load(&Filter::All)?;
    Ok(fold_many(&events).into_values().filter(|t| !t.deleted && t.assignee.as_ref() == Some(me)).collect())
}

pub fn file(ctx: &mut Ctx, request: Value) -> Result<Value> {
    let me = agent_only(&ctx.actor)?;
    let req: FileRequest = parse(&request)?;
    write(ctx, &request, |tx, actor| {
        let explicit = req.project.as_deref().map(str::trim).filter(|p| !p.is_empty());
        if let Some(project) = explicit {
            check_project(project)?;
        }
        let related = if req.related.is_empty() { String::new() } else { resolve(tx, &req.related)? };
        let project = match (explicit, related.is_empty()) {
            (Some(project), _) => project.to_owned(),
            (None, false) => load(tx, &related)?.project,
            (None, true) => {
                let mut own = my_tasks(tx, &me)?;
                if own.len() != 1 {
                    return Err(Refusal::field("missing_field", "project", "missing", "name the project or the related task").into());
                }
                own.remove(0).project
            }
        };
        let backlog = workflow(tx, &project)?.first(Role::Backlog).map(|s| s.name.clone()).unwrap_or_default();
        let mut fields = BTreeMap::new();
        if !req.breaks.trim().is_empty() {
            fields.insert("breaks".to_string(), req.breaks.clone());
        }
        let rank = next_rank(tx, &project, &backlog)?;
        let related_set: BTreeSet<String> = if related.is_empty() { BTreeSet::new() } else { BTreeSet::from([related.clone()]) };
        let payload = created_payload(&req.title, &req.prompt, &backlog, None, &[], None, "", BTreeSet::new(), related_set, vec![], fields, rank)?;
        let uuid = uuid::Uuid::new_v4().to_string();
        prove(tx, Draft { actor: actor.clone(), task: uuid.clone(), project: project.clone(), kind: "task_created".into(), base: String::new(), request_id: req.request_id.clone(), payload })?;
        result(tx, &uuid, "Filed in Backlog for a person to triage. Continue with your current task.")
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct AttachRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    name: String,
    mime: String,
    data_base64: String,
    #[serde(default)]
    note: String,
}

pub fn attach(ctx: &mut Ctx, request: Value) -> Result<Value> {
    let me = agent_only(&ctx.actor)?;
    let req: AttachRequest = parse(&request)?;
    check_text("note", &req.note, MAX_FIELD, true)?;
    let stored = store_attachments(ctx.store, &[NewAttachment { name: req.name.clone(), mime: req.mime.clone(), data_base64: req.data_base64.clone() }])?;
    write(ctx, &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load(tx, &uuid)?;
        if task.assignee.as_ref() != Some(&me) && task.reviewer.as_ref() != Some(&me) {
            return refuse("not_assigned", "this task is not assigned to your session");
        }
        if task.attachments.len() >= 8 {
            return Err(Refusal::field("invalid_field", "attachments", "invalid", "at most 8 images per task").into());
        }
        let attachment = &stored[0];
        let draft = |kind: &str, payload: Value| Draft { actor: actor.clone(), task: uuid.clone(), project: task.project.clone(), kind: kind.into(), base: String::new(), request_id: String::new(), payload };
        prove(tx, draft("attachment_added", json!({"attachment": attachment})))?;
        let text = format!("{} ({}, {} bytes){}", attachment.name, attachment.mime, attachment.bytes, if req.note.is_empty() { String::new() } else { format!("\n{}", req.note) });
        prove(tx, draft("journal", json!({"marker": "[attach]", "text": text})))?;
        result(tx, &uuid, "Attached; reviewers can open it from the task.")
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ReviewRequest {
    #[serde(default)]
    request_id: String,
    task: String,
    verdict: String,
    report: String,
}

pub fn review(ctx: &mut Ctx, request: Value) -> Result<Value> {
    agent_only(&ctx.actor)?;
    let req: ReviewRequest = parse(&request)?;
    let action = match req.verdict.as_str() {
        "approve" => Action::Approve,
        "needs_work" => Action::NeedsWork,
        _ => return Err(Refusal::field("invalid_field", "verdict", "invalid", "verdict is approve or needs_work").into()),
    };
    write(ctx, &request, |tx, actor| {
        let uuid = resolve(tx, &req.task)?;
        let task = load(tx, &uuid)?;
        let wf = workflow(tx, &task.project)?;
        let to = if action == Action::Approve {
            task.stage.clone()
        } else {
            wf.rules.iter().find(|r| r.action == Action::NeedsWork && r.from.contains(&task.stage)).map(|r| r.to.clone()).unwrap_or_default()
        };
        let fields = BTreeMap::from([("report".to_string(), req.report.clone())]);
        transition(tx, actor, &task, action, &to, fields, None, vec![], &req.request_id)?;
        result(tx, &uuid, "Verdict recorded. A person signs the task off.")
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::overmind::commands;
    use crate::overmind::store::Store;
    use base64::Engine;
    use serde_json::json;
    use std::path::PathBuf;
    use std::process::Command;

    const MACHINE: &str = "11111111-1111-4111-8111-111111111111";

    fn setup() -> (tempfile::TempDir, Store, String) {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let mut ctx = Ctx { store: &mut store, actor: Actor::human(MACHINE), cwd: None };
        let task = commands::create(&mut ctx, json!({"project": "p", "prompt": "Do it", "stage": "Queue"})).unwrap()["task"]["uuid"].as_str().unwrap().to_owned();
        // Stage 1b sends tasks; here the claim is written directly.
        store.write(|tx| {
            let current = commands::load(tx, &task)?;
            commands::transition(tx, &Actor::human(MACHINE), &current, Action::Claim, "Design", BTreeMap::new(), Some(SessionRef { machine: MACHINE.into(), name: "w1".into() }), vec![], "")?;
            Ok(())
        }).unwrap();
        (dir, store, task)
    }
    fn agent<'a>(store: &'a mut Store, name: &str, cwd: Option<PathBuf>) -> Ctx<'a> {
        Ctx { store, actor: Actor::agent(MACHINE, name), cwd }
    }
    fn refusal(error: anyhow::Error) -> Refusal {
        error.downcast::<Refusal>().unwrap()
    }

    #[test]
    fn mine_lists_assigned_tasks_with_a_note() {
        let (_dir, mut store, task) = setup();
        let mine_now = mine(&mut agent(&mut store, "w1", None)).unwrap();
        assert_eq!(mine_now["tasks"][0]["uuid"], task);
        assert!(mine_now["note"].as_str().unwrap().contains("one task"));
        assert!(mine(&mut agent(&mut store, "w2", None)).unwrap()["tasks"].as_array().unwrap().is_empty());
    }

    #[test]
    fn advance_through_build_to_review_outside_git() {
        let (_dir, mut store, task) = setup();
        let outside = tempfile::tempdir().unwrap();
        let built = advance(&mut agent(&mut store, "w1", Some(outside.path().into())), json!({"task": task, "to": "Build", "spec": "plan"})).unwrap();
        assert_eq!(built["task"]["stage"], "Build");
        assert!(built["note"].as_str().unwrap().contains("Review"));
        let reviewed = advance(&mut agent(&mut store, "w1", Some(outside.path().into())), json!({"task": task, "to": "Review", "worklog": "did", "evidence": "manual check"})).unwrap();
        assert_eq!(reviewed["task"]["stage"], "Review");
    }

    #[test]
    fn evidence_must_be_an_existing_commit_in_git() {
        let (_dir, mut store, task) = setup();
        let repo = tempfile::tempdir().unwrap();
        let git = |args: &[&str]| assert!(Command::new("git").arg("-C").arg(repo.path()).args(args).output().unwrap().status.success());
        git(&["init", "-q"]);
        git(&["-c", "user.name=t", "-c", "user.email=t@example.com", "commit", "-q", "--allow-empty", "-m", "x"]);
        let head = String::from_utf8(Command::new("git").arg("-C").arg(repo.path()).args(["rev-parse", "HEAD"]).output().unwrap().stdout).unwrap().trim().to_owned();
        let cwd = Some(repo.path().to_path_buf());
        advance(&mut agent(&mut store, "w1", cwd.clone()), json!({"task": task, "to": "Build", "spec": "plan"})).unwrap();
        let text = advance(&mut agent(&mut store, "w1", cwd.clone()), json!({"task": task, "to": "Review", "worklog": "did", "evidence": "trust me"})).unwrap_err();
        assert_eq!((refusal(text).field.as_str()), "evidence");
        let missing = advance(&mut agent(&mut store, "w1", cwd.clone()), json!({"task": task, "to": "Review", "worklog": "did", "evidence": "deadbeefdeadbeef"})).unwrap_err();
        assert_eq!(refusal(missing).state, "invalid");
        advance(&mut agent(&mut store, "w1", cwd), json!({"task": task, "to": "Review", "worklog": "did", "evidence": head})).unwrap();
    }

    #[test]
    fn predecessors_block_review() {
        let (_dir, mut store, task) = setup();
        let mut human = Ctx { store: &mut store, actor: Actor::human(MACHINE), cwd: None };
        let before = commands::create(&mut human, json!({"project": "p", "prompt": "First"})).unwrap()["task"]["uuid"].as_str().unwrap().to_owned();
        commands::link(&mut human, json!({"task": task, "relation": "follows", "target": before, "present": true})).unwrap();
        advance(&mut agent(&mut store, "w1", None), json!({"task": task, "to": "Build", "spec": "plan"})).unwrap();
        let blocked = advance(&mut agent(&mut store, "w1", None), json!({"task": task, "to": "Review", "worklog": "w", "evidence": "e"})).unwrap_err();
        assert_eq!(refusal(blocked).code, "predecessor_open");
    }

    #[test]
    fn call_human_return_and_file() {
        let (_dir, mut store, task) = setup();
        let parked = call_human(&mut agent(&mut store, "w1", None), json!({"task": task, "question": "Which API?"})).unwrap();
        assert_eq!(parked["task"]["stage"], "Your Call");
        assert!(parked["note"].as_str().unwrap().contains("answer"));
        let returned = return_task(&mut agent(&mut store, "w1", None), json!({"task": task, "reason": "no access"})).unwrap();
        assert_eq!(returned["task"]["stage"], "Backlog");
        assert!(returned["task"]["labels"].as_array().unwrap().contains(&json!("blocked")));
        let filed = file(&mut agent(&mut store, "w1", None), json!({"prompt": "Flaky test", "breaks": "CI fails randomly", "related": task})).unwrap();
        assert_eq!(filed["task"]["stage"], "Backlog");
        assert_eq!(filed["task"]["journal"][0]["marker"], "[filed-by-agent]");
        let no_breaks = file(&mut agent(&mut store, "w1", None), json!({"prompt": "x", "related": task})).unwrap_err();
        assert_eq!(refusal(no_breaks).field, "breaks");
    }

    #[test]
    fn decompose_creates_ordered_children_and_an_epic() {
        let (_dir, mut store, task) = setup();
        let result = decompose(&mut agent(&mut store, "w1", None), json!({"task": task, "ordered": true, "subtasks": [{"prompt": "Part one"}, {"prompt": "Part two"}]})).unwrap();
        assert_eq!(result["task"]["stage"], "Backlog");
        assert!(result["task"]["labels"].as_array().unwrap().contains(&json!("epic")));
        let children = result["children"].as_array().unwrap();
        assert_eq!(children.len(), 2);
        assert_eq!(children[0]["stage"], "Queue");
        assert_eq!(children[1]["follows"], json!([children[0]["uuid"]]));
        let one = decompose(&mut agent(&mut store, "w2", None), json!({"task": task, "subtasks": [{"prompt": "a"}, {"prompt": "b"}]})).unwrap_err();
        assert_eq!(refusal(one).code, "not_assigned");
    }

    #[test]
    fn attach_and_review_need_the_right_session() {
        let (_dir, mut store, task) = setup();
        let png = base64::engine::general_purpose::STANDARD.encode(b"\x89PNG");
        let attached = attach(&mut agent(&mut store, "w1", None), json!({"task": task, "name": "shot.png", "mime": "image/png", "data_base64": png, "note": "after"})).unwrap();
        assert_eq!(attached["task"]["attachments"][0]["name"], "shot.png");
        assert_eq!(refusal(attach(&mut agent(&mut store, "w2", None), json!({"task": task, "name": "x.png", "mime": "image/png", "data_base64": png})).unwrap_err()).code, "not_assigned");
        advance(&mut agent(&mut store, "w1", None), json!({"task": task, "to": "Build", "spec": "s"})).unwrap();
        advance(&mut agent(&mut store, "w1", None), json!({"task": task, "to": "Review", "worklog": "w", "evidence": "e"})).unwrap();
        let own = review(&mut agent(&mut store, "w1", None), json!({"task": task, "verdict": "approve", "report": "fine"})).unwrap_err();
        assert_eq!(refusal(own).code, "not_reviewer");
        store.write(|tx| {
            let current = commands::load(tx, &task)?;
            commands::prove(tx, Draft { actor: Actor::human(MACHINE), task: task.clone(), project: current.project.clone(), kind: "reviewer_set".into(), base: String::new(), request_id: String::new(), payload: json!({"session": {"machine": MACHINE, "name": "w2"}}) })?;
            Ok(())
        }).unwrap();
        let failed = review(&mut agent(&mut store, "w2", None), json!({"task": task, "verdict": "needs_work", "report": "tests missing"})).unwrap();
        assert_eq!(failed["task"]["stage"], "Build");
        assert!(failed["task"]["labels"].as_array().unwrap().contains(&json!("review-failed")));
        let bad = review(&mut agent(&mut store, "w2", None), json!({"task": task, "verdict": "maybe", "report": "?"})).unwrap_err();
        assert_eq!(refusal(bad).field, "verdict");
    }

    #[test]
    fn file_validates_and_resolves_the_project() {
        let (_dir, mut store, task) = setup();
        let filed = file(&mut agent(&mut store, "w1", None), json!({"prompt": "a", "breaks": "b", "project": "  ", "related": task})).unwrap();
        assert_eq!(filed["task"]["project"], "p");
        let alone = file(&mut agent(&mut store, "w1", None), json!({"prompt": "a", "breaks": "b", "project": ""})).unwrap();
        assert_eq!(alone["task"]["project"], "p");
        let bad = file(&mut agent(&mut store, "w1", None), json!({"prompt": "a", "breaks": "b", "project": "x\u{7}y"})).unwrap_err();
        assert_eq!(refusal(bad).field, "project");
    }

    #[test]
    fn illegal_jump_reports_the_rule_and_reviewed_predecessor_unblocks() {
        let (_dir, mut store, task) = setup();
        let jump = advance(&mut agent(&mut store, "w1", None), json!({"task": task, "to": "Review", "worklog": "w", "evidence": "e"})).unwrap_err();
        assert_eq!(refusal(jump).code, "transition_not_allowed");
        let mut human = Ctx { store: &mut store, actor: Actor::human(MACHINE), cwd: None };
        let before = commands::create(&mut human, json!({"project": "p", "prompt": "First", "stage": "Queue"})).unwrap()["task"]["uuid"].as_str().unwrap().to_owned();
        commands::link(&mut human, json!({"task": task, "relation": "follows", "target": before, "present": true})).unwrap();
        store.write(|tx| {
            let current = commands::load(tx, &before)?;
            commands::transition(tx, &Actor::human(MACHINE), &current, Action::Claim, "Design", BTreeMap::new(), Some(SessionRef { machine: MACHINE.into(), name: "w2".into() }), vec![], "")?;
            Ok(())
        }).unwrap();
        advance(&mut agent(&mut store, "w2", None), json!({"task": before, "to": "Build", "spec": "s"})).unwrap();
        advance(&mut agent(&mut store, "w2", None), json!({"task": before, "to": "Review", "worklog": "w", "evidence": "e"})).unwrap();
        advance(&mut agent(&mut store, "w1", None), json!({"task": task, "to": "Build", "spec": "plan"})).unwrap();
        let ok = advance(&mut agent(&mut store, "w1", None), json!({"task": task, "to": "Review", "worklog": "w", "evidence": "e"})).unwrap();
        assert_eq!(ok["task"]["stage"], "Review");
    }

    #[test]
    fn agents_cannot_touch_done_tasks() {
        let (_dir, mut store, task) = setup();
        advance(&mut agent(&mut store, "w1", None), json!({"task": task, "to": "Build", "spec": "s"})).unwrap();
        advance(&mut agent(&mut store, "w1", None), json!({"task": task, "to": "Review", "worklog": "w", "evidence": "e"})).unwrap();
        let mut human = Ctx { store: &mut store, actor: Actor::human(MACHINE), cwd: None };
        commands::done(&mut human, json!({"task": task})).unwrap();
        let err = attach(&mut agent(&mut store, "w1", None), json!({"task": task, "name": "x.png", "mime": "image/png", "data_base64": "iVBO"})).unwrap_err();
        assert_eq!(refusal(err).code, "task_closed");
        let err = advance(&mut agent(&mut store, "w1", None), json!({"task": task, "to": "Build", "spec": "s"})).unwrap_err();
        assert!(matches!(refusal(err).code.as_str(), "task_closed" | "transition_not_allowed"));
    }
}
