//! The phone's task actions through the relay: Send now, Cancel and the Done
//! list (docs/superpowers/specs/2026-10-18-task-replication-loro-design.md,
//! "Phone"). Each reads one JSON request on stdin, runs the same send and
//! cancel as `hgs task`, and answers compactly: the phone exchanges the
//! project to see the task itself.
use super::{change_within, fail, now, project, Result};
use crate::config::Config;
use serde::de::DeserializeOwned;
use serde::Deserialize;
use serde_json::{json, Value};
use std::io::Read;
use std::time::Duration;
use zerus_replica::{ids, ProjectStore, SessionKey};

/// The most a request may be: a cancel reason is the longest field.
const MAX_REQUEST: u64 = 64 * 1024;
/// The most Done tasks one page lists.
const PAGE: usize = 50;
/// The Done tasks counted as recent: the last 7 days, as the desktop shows them.
const RECENT_MS: i64 = 7 * 24 * 3600 * 1000;
/// How long a phone command waits for a project another command holds.
const PROJECT_WAIT: Duration = Duration::from_secs(2);

fn request<T: DeserializeOwned>() -> Result<T> {
    let mut bytes = Vec::new();
    std::io::stdin()
        .take(MAX_REQUEST + 1)
        .read_to_end(&mut bytes)
        .map_err(fail)?;
    if bytes.len() as u64 > MAX_REQUEST {
        return Err(fail("the request exceeds 64 KiB"));
    }
    let value: Value =
        serde_json::from_slice(&bytes).map_err(|e| fail(format!("invalid request: {e}")))?;
    match value.get("schema").and_then(Value::as_u64) {
        Some(1) => {}
        Some(other) => return Err(fail(format!("unknown request schema {other}"))),
        None => return Err(fail("invalid request: missing schema")),
    }
    serde_json::from_value(value).map_err(|e| fail(format!("invalid request: {e}")))
}

/// Phone strings become hgs arguments: none may look like an option.
fn plain(value: &str, refusal: impl FnOnce() -> String) -> Result<()> {
    if value.starts_with('-') && !value.contains(char::is_whitespace) {
        Err(fail(refusal()))
    } else {
        Ok(())
    }
}

fn task_uuid(value: &str) -> Result<()> {
    if ids::is_uuid(value) {
        Ok(())
    } else {
        Err(fail(format!("{value:?} is not a task UUID")))
    }
}

#[derive(Deserialize)]
struct Send {
    project: String,
    task: String,
    session: String,
}

#[derive(Deserialize)]
struct Cancel {
    project: String,
    task: String,
    #[serde(default)]
    reason: Option<String>,
}

#[derive(Deserialize)]
struct Cursor {
    at: i64,
    uuid: String,
}

#[derive(Deserialize)]
struct Done {
    project: String,
    #[serde(default)]
    before: Option<Cursor>,
    limit: usize,
    /// Only the tasks sent to this session (Only this session).
    #[serde(default)]
    session: Option<SessionKey>,
}

/// `hgs task mobile-send --json`: pastes the task into a session this computer
/// runs, as `hgs task send TASK --session SESSION` does.
pub(super) fn send(config: &Config) -> Result<Value> {
    let request: Send = request()?;
    task_uuid(&request.task)?;
    if request.session.trim().is_empty() || request.session.starts_with('-') {
        return Err(fail(format!("{:?} is not a session name", request.session)));
    }
    plain(&request.project, || format!("{:?} is not a project", request.project))?;
    let args = [
        request.task,
        "--project".into(),
        request.project,
        "--session".into(),
        request.session,
    ];
    let out = change_within(config, "send", &args, Some(PROJECT_WAIT))?;
    // The receipt's identity and status, never the prompt it carries: a
    // mutation's receipt stays in the relay for days.
    let receipt = &out["receipt"];
    Ok(json!({"schema": 1, "ok": true, "task": out["task"]["uuid"], "stage": out["task"]["stage"],
              "session": out["task"]["session"],
              "receipt": {"request_id": receipt["request_id"], "status": receipt["status"],
                          "name": receipt["name"], "run_id": receipt["run_id"],
                          "conversation_id": receipt["conversation_id"], "submitted_at": receipt["submitted_at"]}}))
}

/// `hgs task mobile-cancel --json`: cancels the task as `hgs task cancel` does.
pub(super) fn cancel(config: &Config) -> Result<Value> {
    let request: Cancel = request()?;
    task_uuid(&request.task)?;
    plain(&request.project, || format!("{:?} is not a project", request.project))?;
    let mut args = vec![request.task, "--project".into(), request.project];
    if let Some(reason) = request.reason.filter(|reason| !reason.is_empty()) {
        plain(&reason, || format!("a cancel reason cannot look like an option ({reason:?})"))?;
        args.extend(["--reason".into(), reason]);
    }
    let out = change_within(config, "cancel", &args, Some(PROJECT_WAIT))?;
    Ok(json!({"schema": 1, "ok": true, "task": out["task"]["uuid"], "stage": out["task"]["stage"],
              "cancel_message": out["cancel_message"]}))
}

/// `hgs task mobile-done --json`: one page of the project's Done tasks, newest
/// first, or of those sent to one session.
pub(super) fn done(config: &Config) -> Result<Value> {
    let request: Done = request()?;
    if !(1..=PAGE).contains(&request.limit) {
        return Err(fail(format!("a Done page holds 1 to {PAGE} tasks")));
    }
    if let Some(cursor) = &request.before {
        task_uuid(&cursor.uuid)?;
    }
    let (id, _) = project(config, &request.project)?;
    let store = ProjectStore::open_within(&config.state.join("replica"), &id, PROJECT_WAIT)
        .map_err(fail)?
        .ok_or_else(|| fail("the project's tasks are busy; try again"))?;
    let page = store
        .done_page(
            request.before.map(|cursor| (cursor.at, cursor.uuid)),
            request.limit,
            now() - RECENT_MS,
            request.session.as_ref(),
        )
        .map_err(fail)?;
    Ok(json!({"schema": 1, "tasks": page.tasks,
              "next": page.next.map(|(at, uuid)| json!({"at": at, "uuid": uuid})),
              "total": page.total, "recent": page.recent, "damaged": page.damaged}))
}
