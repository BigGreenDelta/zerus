//! Durable agent/session bindings. Histories remain owned by the agent CLIs.
//!
//! Version-one manifests and journal rows deliberately remain compatible with
//! the original helper: an upgrade must never orphan a saved conversation.
mod answer_channel;
mod archive;
pub(crate) mod archive_catalog;
mod claude_trust;
mod claude_permission;
mod claude_resume;
mod claude_usage_limit;
mod claude_question;
mod claude_statusline;
mod claude_subagents;
mod attachments;
mod computer_files;
pub(crate) mod voice;
mod interrupt;
mod clear_context;
mod compact_context;
mod cache_hint;
mod claude_approval;
mod attempts;
pub(crate) mod backend;
pub(crate) mod dsh;
pub(crate) mod opencode;
mod effort;
mod external_agents;
mod fork;
mod goals;
mod hooks;
pub(crate) mod hook_health;
mod input;
mod input_queue;
mod journal;
mod kimi_cache_hint;
mod kimi_messages;
mod kimi_approval;
mod kimi_plan;
mod kimi_trust;
mod kimi_tui_choice;
mod lifecycle;
mod provider_errors;
mod recovery;
mod service_worker;
mod session_limits;
mod provider_messages;
mod processes;
mod prompt_suggestion;
mod question_terminal;
mod questions;
mod codex_questions;
mod codex_trust;
mod codex_hooks_trust;
mod codex_update;
mod codex_goal_resume;
mod read_marks;
mod notifications;
mod recipes;
mod rename;
mod resume_input;
mod scan_cache;
mod schedules;
mod waits;
mod search;
pub(crate) mod session_graph;
mod storage;
mod subagents;
mod native_subagents;
mod supervisor;
mod tasks;
mod telemetry;
mod terminate;
mod session_action;
mod shown_path;
mod startup_choice;
mod terminal;
mod conversation_history;
mod usage;
mod workspace;
mod git_status;
mod worktrees;
mod worktree_cleanup;
mod worktree_inspection;
mod worktree_lifecycle;

/// Current bounded native Git evidence for exact project/worktree placement.
pub(crate) fn worktree_catalog(path: &std::path::Path) -> serde_json::Value {
    if let Err(detail) = local_sessions() {
        return json!({"state":"unavailable","detail":detail,"stale":true,"worktrees":[]});
    }
    worktrees::catalog(path, true)
}
mod launch_project;
pub(crate) use launch_project::with_launch_binding;
pub(crate) mod launches;

use serde_json::{json, Value};
use std::collections::BTreeMap;
use std::io::{self, Read};
use std::path::{Path, PathBuf};
use storage::*;

/// The state directory the Windows session host serves (`HGS_STATE_DIR`, else
/// the default below the home directory), not yet created or canonical.
#[cfg(windows)]
pub(crate) fn host_state_root() -> PathBuf {
    storage::root()
}

pub(super) type Result<T> = std::result::Result<T, String>;
pub(super) const AGENTS: &[&str] = &["claude", "codex", "kimi"];

/// The entry points other modules call fail first where this computer runs no
/// local sessions (a Windows control console), as they always have there.
pub(super) fn local_sessions() -> Result<()> {
    if crate::platform::local_sessions() {
        Ok(())
    } else {
        Err(crate::platform::LOCAL_SESSIONS_UNSUPPORTED.into())
    }
}

/// `local_sessions` for `dispatch`, which the processes of a session reach:
/// `hgs __state` from its tracking hooks and a tracked `--run` supervisor. On
/// Windows it also admits a session of an isolated test host, which may run
/// where local sessions are off (`host::gate::test_session`, spec D2); every
/// other entry point stays a control console's there.
pub(crate) fn session_operations() -> Result<()> {
    let enabled = local_sessions();
    #[cfg(windows)]
    if enabled.is_err() && crate::host::gate::test_session() {
        return Ok(());
    }
    enabled
}

/// Whether the automatic recovery worker holds its lock; unknown without local
/// sessions.
pub(crate) fn recovery_worker_running() -> Option<bool> {
    local_sessions().ok()?;
    Some(crate::status::locked(&root().join("recovery/worker.lock")))
}

pub(crate) fn account_context(name: &str) -> Result<Value> {
    local_sessions()?;
    if opencode::exists(name) { return opencode::account_context(name); }
    if dsh::exists(name) {
        return dsh::account_context(name);
    }
    let _guard = lock(None)?;
    let record = read(name)?;
    let snapshot = live()?;
    if snapshot.contains_key(name) && !matches(&record, snapshot.get(name)) {
        return Err("Session account binding is no longer current.".into());
    }
    Ok(account_binding(&record))
}

fn account_binding(record: &Value) -> Value {
    let provider = string(record, "agent");
    let id = if string(record, "account_id").is_empty() {
        format!("native-{provider}")
    } else {
        string(record, "account_id").to_owned()
    };
    let agent_home = if string(record, "agent_home").is_empty() {
        home().join(if provider == "kimi" {
            ".kimi-code".into()
        } else {
            format!(".{provider}")
        })
    } else {
        PathBuf::from(string(record, "agent_home"))
    };
    json!({"provider":provider,"id":id,"home":agent_home,"run_id":record["run_id"]})
}

pub(super) fn string<'a>(value: &'a Value, key: &str) -> &'a str {
    value.get(key).and_then(Value::as_str).unwrap_or("")
}

pub(super) fn home_var(agent: &str) -> Result<&'static str> {
    match agent {
        "claude" => Ok("CLAUDE_CONFIG_DIR"),
        "codex" => Ok("CODEX_HOME"),
        "kimi" => Ok("KIMI_CODE_HOME"),
        _ => Err(format!("unsupported agent: {agent}")),
    }
}

pub(super) fn now() -> f64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs_f64()
}

pub(super) fn stdin_json() -> Result<Value> {
    let mut input = String::new();
    io::stdin()
        .read_to_string(&mut input)
        .map_err(|e| e.to_string())?;
    serde_json::from_str(&input).map_err(|e| e.to_string())
}

pub(super) fn nonempty_env(name: &str) -> Option<String> {
    std::env::var(name).ok().filter(|value| !value.is_empty())
}

pub(super) fn home() -> PathBuf {
    crate::platform::home_dir().unwrap_or_else(|| "/".into())
}

fn directories(path: &str, hidden: bool) -> Result<Value> {
    if path.chars().any(|c| c < ' ') {
        return Err("directory paths cannot contain control characters".into());
    }
    let expanded = if path == "~" {
        home()
    } else if let Some(rest) = path.strip_prefix("~/") {
        home().join(rest)
    } else {
        PathBuf::from(path)
    };
    let directory = crate::platform::canonical(&expanded).map_err(|e| format!("{path}: {e}"))?;
    if !directory.is_dir() {
        return Err(format!("not a directory: {path}"));
    }
    let mut entries = Vec::new();
    for entry in std::fs::read_dir(&directory).map_err(|e| e.to_string())? {
        let Ok(entry) = entry else { continue };
        let Some(name) = entry.file_name().to_str().map(str::to_owned) else {
            continue;
        };
        if (!hidden && name.starts_with('.')) || name.chars().any(|c| c < ' ') {
            continue;
        }
        if entry.path().is_dir() {
            entries.push(json!({"name": name, "path": entry.path(),
                "symlink": entry.file_type().map(|kind| kind.is_symlink()).unwrap_or(false)}));
        }
    }
    entries.sort_by(|a, b| {
        string(a, "name")
            .to_lowercase()
            .cmp(&string(b, "name").to_lowercase())
            .then_with(|| string(a, "name").cmp(string(b, "name")))
    });
    let truncated = entries.len() > 2000;
    entries.truncate(2000);
    Ok(
        json!({"path": directory, "parent": directory.parent().unwrap_or(Path::new("/")),
        "home": home(), "directories": entries, "truncated": truncated}),
    )
}

fn create_directory(path: &str, request_id: &str) -> Result<Value> {
    if path.is_empty() || path.len() > 4096 || path.chars().any(|c| c.is_control()) {
        return Err("directory paths must be nonempty, at most 4096 UTF-8 bytes and free of control characters".into());
    }
    let leaf = path.rsplit(|c| c == '/' || (cfg!(windows) && c == '\\')).next().unwrap_or("");
    if matches!(leaf, "" | "." | "..") {
        return Err("choose a new folder name without a trailing separator".into());
    }
    if let Some(rest) = path.strip_prefix("~/") {
        let relative = Path::new(rest);
        if relative.has_root() || matches!(relative.components().next(), Some(std::path::Component::Prefix(_))) {
            return Err("home-relative paths cannot start with a root or drive prefix".into());
        }
    }
    if !uuid::Uuid::parse_str(request_id)
        .is_ok_and(|id| !id.is_nil() && id.to_string() == request_id)
    {
        return Err("--request-id must be a canonical nonempty UUID".into());
    }
    let expanded = if path == "~" {
        home()
    } else if let Some(rest) = path.strip_prefix("~/") {
        home().join(rest)
    } else {
        PathBuf::from(path)
    };
    expanded.file_name().ok_or("choose a new folder name")?;
    let parent = expanded.parent().filter(|p| !p.as_os_str().is_empty()).unwrap_or(Path::new("."));
    let parent = crate::platform::canonical(parent)
        .map_err(|e| format!("parent folder must already exist: {e}"))?;
    if !parent.is_dir() {
        return Err("parent is not a directory".into());
    }
    // One atomic mkdir: never overwrite, create intermediate parents or retry.
    std::fs::create_dir(&expanded).map_err(|e| format!("{path}: {e}"))?;
    let directory = crate::platform::canonical(&expanded)
        .map_err(|e| format!("folder creation could not be confirmed; inspect {path}: {e}"))?;
    Ok(json!({"status":"created", "request_id":request_id,
        "requested_path":path, "path":directory, "parent":parent}))
}

#[cfg(all(test, windows))]
#[test]
fn home_relative_creation_rejects_windows_roots_and_drive_prefixes_before_effects() {
    for path in ["~//root/child", "~/\\root\\child", "~/C:/child", "~/C:child"] {
        let error = create_directory(path, "11111111-1111-4111-8111-111111111111").unwrap_err();
        assert!(error.contains("root or drive prefix"), "{path}: {error}");
    }
}

/// Enrich the CLI's tmux snapshot without a subprocess or a JSON stdin pipe.
pub fn merge_snapshot(box_value: Value) -> Result<Value> {
    merge_listing(box_value, &BTreeMap::new())
}

/// `merge_snapshot` of a listing; `ids` holds each listed session's immutable
/// ID (`$N`) by its listed name.
pub fn merge_listing(box_value: Value, ids: &BTreeMap<String, String>) -> Result<Value> {
    merge_listing_archives(box_value, ids, true)
}

pub fn merge_listing_archives(mut box_value: Value, ids: &BTreeMap<String, String>, include_archives: bool) -> Result<Value> {
    local_sessions()?;
    // A rename renames the terminal session and moves its binding under the
    // state lock. Read the panes and the saved records under it as well, so a
    // listing sees a rename before it began or after it finished, never a
    // pane under the new name with its binding still under the old one: that
    // session would list twice (zerus-gd02). Archive reconciliation already
    // read both under this lock; the listing reuses those reads and releases
    // the lock before it summarizes the sessions.
    let ((snapshot, names), (archived, bindings)) = if root().exists() {
        let _guard = lock(None)?;
        let (snapshot, names) = live_sessions()?;
        let saved = if include_archives { archive::reconcile_locked(&snapshot)? } else { (Vec::new(), archive::reconcile_active_locked(&snapshot)?) };
        ((snapshot, names), saved)
    } else {
        (live_sessions()?, Default::default())
    };
    let marks = read_marks::load();
    let notification_preferences = notifications::load();
    let sessions = box_value
        .get_mut("sessions")
        .and_then(Value::as_array_mut)
        .ok_or("invalid session snapshot")?;
    // The terminal listed these sessions before `snapshot`. Every live session
    // has a pane, so one that the later snapshot lacks ended in between: it is
    // not live, and its saved binding lists below with a state instead of as
    // an untracked live row. A session renamed in between keeps its ID: it
    // lists once, under the name that `snapshot` and its binding now hold.
    let current = |listed: &str| ids.get(listed).and_then(|id| names.get(id)).cloned();
    let held: std::collections::BTreeSet<String> = sessions
        .iter()
        .filter_map(|session| current(string(session, "name")))
        .collect();
    sessions.retain_mut(|session| {
        let listed = string(session, "name").to_owned();
        match current(&listed) {
            Some(name) => {
                if name != listed {
                    let mut parts = name.splitn(3, '/');
                    session["cmd"] = json!(parts.next().unwrap_or(""));
                    session["project"] = json!(parts.next().filter(|s| !s.is_empty()));
                    session["tag"] = json!(parts.next().filter(|s| !s.is_empty()));
                    session["name"] = json!(name);
                }
                true
            }
            // Unknown or ended ID: the name decides, unless another listed
            // session was renamed to it.
            None => snapshot.contains_key(&listed) && !held.contains(&listed),
        }
    });
    let mut by_name = BTreeMap::new();
    for (index, session) in sessions.iter().enumerate() {
        by_name.insert(string(session, "name").to_owned(), index);
    }
    for session in sessions.iter_mut() {
        session
            .as_object_mut()
            .ok_or("invalid session")?
            .extend(telemetry::unavailable().as_object().unwrap().clone());
    }
    // Bounds question indexing for the whole listing.
    let budget = read_marks::question_budget();
    let mut process_work = processes::WorkSample::default();
    for record in bindings {
        if archive::equivalent(&record, &archived) {
            continue;
        }
        let name = string(&record, "name");
        if let Some(index) = by_name.get(name) {
            if matches(&record, snapshot.get(name)) {
                // An approval answered in Terminal sends no hook.
                let record = claude_approval::reconcile(record);
                let session = &mut sessions[*index];
                session["resumable"] = json!(
                    !string(&record, "conversation_id").is_empty()
                        && string(&record, "error").is_empty()
                );
                let summary = journal::summary(&record, true);
                session["process_work"] = process_work.summary(&record, summary["activity"] == "idle" && summary["phase"] == "idle");
                session
                    .as_object_mut()
                    .ok_or("invalid session")?
                    .extend(summary.as_object().ok_or("invalid summary")?.clone());
                let questions = read_marks::pending_questions(&record, session, true, budget);
                read_marks::enrich_listing(session, &marks, journal::journal_name(&record), questions);
                notifications::enrich(session, notification_preferences.as_ref(), journal::journal_name(&record));
            }
            continue;
        }
        if string(&record, "conversation_id").is_empty() {
            continue;
        }
        // A record whose name was reused by a different live pane is not a
        // stopped session. Never offer its old identity as that pane's identity.
        if snapshot.contains_key(name) {
            continue;
        }
        let parts: Vec<&str> = name.splitn(3, '/').collect();
        let mut session = json!({"name": name, "cmd": record["agent"],
            "project": parts.get(1), "tag": parts.get(2), "attached": 0, "clients": [],
            "created": record["created"], "state": if record["paused"].as_bool().unwrap_or(false) {"paused"} else {"stopped"},
            "resumable": true});
        session.as_object_mut().unwrap().extend(
            journal::summary(&record, false)
                .as_object()
                .unwrap()
                .clone(),
        );
        read_marks::enrich_listing(&mut session, &marks, journal::journal_name(&record), Ok(Some(Vec::new())));
        notifications::enrich(&mut session, notification_preferences.as_ref(), journal::journal_name(&record));
        sessions.push(session);
    }
    for record in archived {
        let name = string(&record, "name");
        let parts: Vec<&str> = name.splitn(3, '/').collect();
        let mut session = json!({"name": name, "cmd": record["agent"], "project": parts.get(1),
            "tag": parts.get(2), "attached": 0, "clients": [], "created": record["created"],
            "state": "archived", "resumable": !string(&record, "conversation_id").is_empty(),
            "archive_id": record["archive_id"], "archived_at": record["archived_at"]});
        session.as_object_mut().unwrap().extend(
            journal::summary(&record, false)
                .as_object()
                .unwrap()
                .clone(),
        );
        read_marks::enrich_listing(&mut session, &marks, journal::journal_name(&record), Ok(Some(Vec::new())));
        notifications::enrich(&mut session, notification_preferences.as_ref(), journal::journal_name(&record));
        sessions.push(session);
    }
    sessions.extend(dsh::snapshots()?);
    sessions.extend(opencode::snapshots()?);
    // DeepSeek and untracked live sessions are owned by their names.
    for session in sessions.iter_mut() {
        if session.get("attention_signature").is_none() {
            let owner = string(session, "name").to_owned();
            let questions=if session["agent"]=="opencode" {session["pending_questions"].as_array().cloned().unwrap_or_default()}else{Vec::new()};
            read_marks::enrich_listing(session, &marks, &owner, Ok(Some(questions)));
        }
    }

    for session in sessions.iter_mut() {
        if session.get("notification_preferences").is_none() {
            session["notification_preferences"] = Value::Null;
        }
    }

    // The pane folders are read after the lock is released, when a session may
    // have been renamed since `snapshot`. They go by the session IDs, which a
    // rename keeps, to the names that `snapshot` listed (zerus-18u1).
    workspace::enrich(sessions, &workspace::snapshot_cwds(&names));
    waits::decorate(sessions);
    Ok(box_value)
}

/// Select a tracked conversation or explicit pending request by exact ID/home.
/// Archives are separate instances and require an explicit archive restore.
pub fn find_requested_session(agent: &str, requested: &str) -> Result<Option<String>> {
    local_sessions()?;
    archive::reconcile()?;
    let _guard = lock(None)?;
    let snapshot = live()?;
    let archived = archive::records()?;
    let mut names = Vec::new();
    for record in records()? {
        if string(&record, "agent") != agent
            || requested.is_empty()
            || archive::equivalent(&record, &archived)
            || !attempts::agent_home_matches(&record, agent)?
        {
            continue;
        }
        let name = string(&record, "name");
        if snapshot.contains_key(name) && !matches(&record, snapshot.get(name)) {
            continue;
        }
        let confirmed = string(&record, "conversation_id") == requested;
        let pending = string(&record, "conversation_id").is_empty()
            && string(&record, "requested_id") == requested
            && string(&record, "expected_id") == requested
            && if snapshot.contains_key(name) {
                string(&record, "error").is_empty()
            } else {
                attempts::retryable(&record, Some(requested))
            };
        if !confirmed && !pending {
            continue;
        }
        names.push(name.to_owned());
    }
    match names.len() {
        0 => Ok(None),
        1 => Ok(names.pop()),
        _ => Err(format!(
            "conversation {requested} is tracked under multiple names ({}); choose one with -n",
            names.join(", ")
        )),
    }
}

pub fn startup_failure(name: &str) -> Result<Option<String>> {
    local_sessions()?;
    attempts::startup_failure(name)
}

/// True means an exact confirmed binding; false is a matching pending request.
/// A live name must still belong to the same pane/run, not merely an old record.
pub fn check_requested_session(name: &str, agent: &str, requested: &str) -> Result<bool> {
    local_sessions()?;
    let _guard = lock(None)?;
    let snapshot = live()?;
    if !record_path(name).exists() {
        return if snapshot.contains_key(name) {
            Err(format!(
                "{name}: live session is not tracked; choose another name"
            ))
        } else {
            Ok(false)
        };
    }
    let record = read(name)?;
    let is_live = snapshot.contains_key(name);
    if string(&record, "agent") != agent
        || !attempts::agent_home_matches(&record, agent)?
        || (is_live && !matches(&record, snapshot.get(name)))
    {
        return Err(format!("{name}: existing session belongs to a different or untracked context; choose another name"));
    }
    if !requested.is_empty() && string(&record, "conversation_id") == requested {
        if is_live && !string(&record, "error").is_empty() {
            return Err(format!(
                "{name}: live conversation identity is unconfirmed; choose another name"
            ));
        }
        return Ok(true);
    }
    let pending_matches = !requested.is_empty()
        && string(&record, "conversation_id").is_empty()
        && string(&record, "expected_id") == requested
        && string(&record, "requested_id") == requested;
    if pending_matches
        && ((is_live && string(&record, "error").is_empty())
            || (!is_live && attempts::retryable(&record, Some(requested))))
    {
        return Ok(false);
    }
    Err(format!("{name}: existing session does not match requested conversation {requested}; choose another name"))
}

pub fn fork_session(
    name: &str,
    tag: Option<&str>,
    archive_id: Option<&str>,
    expected_run: Option<&str>,
    expected_conversation: Option<&str>,
    dry: bool,
) -> Result<String> {
    local_sessions()?;
    fork::start(
        name,
        tag,
        archive_id,
        expected_run,
        expected_conversation,
        dry,
    )
}

pub(crate) fn reference_details(name: &str, archive: &str, agent: &str) -> Result<Value> {
    local_sessions()?;
    if opencode::exists(name) { if !archive.is_empty() {return Err("OpenCode archives are unavailable".into());} return opencode::inspect(name, if agent.is_empty() {None} else {Some(agent)}); }
    if dsh::exists(name) {
        if !archive.is_empty() { return Err("DeepSeek archives are unavailable".into()); }
        return dsh::inspect(name, if agent.is_empty() { None } else { Some(agent) });
    }
    let archive = if archive.is_empty() { None } else { Some(archive) };
    if agent.is_empty() { journal::inspection(name, 0, archive, false) }
    else { subagents::inspection(name, agent, archive) }
}

pub(crate) fn inspect_reference(reference: &crate::session_reference::Reference) -> Result<Value> {
    local_sessions()?;
    let matches_reference = |record: &Value| {
        string(record, "name").split('/').next() == reference.name.split('/').next()
            && string(record, "archive_id") == reference.archive
            && if reference.conversation.is_empty() {
                string(record, "run_id") == reference.run && string(record, "conversation_id").is_empty()
            } else { string(record, "conversation_id") == reference.conversation }
    };
    let name = {
        let _guard = lock(None)?;
        let mut candidates = if reference.archive.is_empty() { records()? } else { archive::records()? };
        if reference.archive.is_empty() { candidates.extend(dsh::snapshots()?); candidates.extend(opencode::snapshots()?); }
        let mut matching = candidates.iter().filter(|record| matches_reference(record));
        let first = matching.next().ok_or("The referenced conversation is no longer bound to a saved session; it may have been cleared or forgotten")?;
        if matching.next().is_some() { return Err("Ambiguous session reference: multiple saved bindings match this conversation".into()); }
        string(first, "name").to_owned()
    };
    let parent = reference_details(&name, &reference.archive, "")?;
    if !matches_reference(&parent) { return Err("Session identity changed while resolving its reference".into()); }
    let mut output = if reference.agent.is_empty() { parent }
        else { reference_details(&name, &reference.archive, &reference.agent)? };
    if !reference.agent.is_empty() && output["parent_conversation_id"].as_str() != Some(reference.conversation.as_str()) {
        return Err("Parent conversation changed while resolving its child reference".into());
    }
    output["session_reference"] = json!({"machine_id":reference.machine,"host":reference.host,"original_name":reference.name,
        "name":name,"conversation_id":reference.conversation,"agent_id":reference.agent});
    Ok(output)
}

/// Compatibility entry point used by `hgs __state` and the legacy hook shim.
pub fn dispatch(args: &[String]) -> Result<i32> {
    // Coordination metadata never starts or controls a native session host.
    if args.first().map(String::as_str)==Some("wait") { return waits::dispatch(&args[1..]); }
    session_operations()?;
    let Some((command, args)) = args.split_first() else {
        return Err("missing state operation".into());
    };
    if command == "worktrees" { return worktrees::dispatch(args); }
    if command == "git-status" { return git_status::dispatch(args); }
    if command == "dirs" {
        if args.first().map(String::as_str) == Some("--create") {
            let request_id;
            let (path, id) = match args {
                [_, path] => {
                    request_id = uuid::Uuid::new_v4().to_string();
                    (path, request_id.as_str())
                }
                [_, path, option, id] if option == "--request-id" => (path, id.as_str()),
                _ => return Err("usage: hgs dirs --create <path> [--request-id UUID]".into()),
            };
            println!("{}", create_directory(path, id)?);
            return Ok(0);
        }
        let hidden = args.first().map(String::as_str) == Some("--hidden");
        let paths = if hidden { &args[1..] } else { args };
        if paths.len() > 1 {
            return Err("usage: hgs dirs [--hidden] [path]".into());
        }
        println!(
            "{}",
            directories(paths.first().map(String::as_str).unwrap_or("~"), hidden)?
        );
        return Ok(0);
    }
    if command == "new-tag" {
        println!("work-{}", &uuid::Uuid::new_v4().simple().to_string()[..8]);
        return Ok(0);
    }
    // Launch arguments belong to the agent, including a possible --dry-run.
    if command == "run" {
        return lifecycle::run(args);
    }
    if command == "hook" {
        return hooks::hook();
    }
    if command == "answer-channel" {
        return answer_channel::run();
    }
    if command == "graph" { return session_graph::dispatch(args); }
    if command == "statusline" {
        return claude_statusline::run();
    }
    if command == "voice" { return voice::dispatch(args); }
    if command == "file-read" { return computer_files::dispatch(args); }
    if command == "attachment" {
        return attachments::dispatch(args);
    }
    if command == "recovery" {
        return recovery::dispatch(args);
    }
    if command == "read" { return read_marks::dispatch(args); }
    if command == "notifications" { return notifications::dispatch(args); }
    if command == "history" { return conversation_history::dispatch(args); }
    if command == "terminal" { return terminal::dispatch(args); }
    if command == "session-action" { return session_action::dispatch(args); }
    if command == "compact-context" { return compact_context::dispatch(args); }
    if command == "clear-context" { return clear_context::dispatch(args); }
    if command == "interrupt" { return interrupt::dispatch(args); }
    if command == "processes" { return processes::dispatch(args); }
    if args.first().is_some_and(|name| opencode::exists(name)) { return opencode::dispatch(command,args); }
    if args.first().is_some_and(|name| dsh::exists(name)) {
        return dsh::dispatch(command, args);
    }
    if command == "settings" {
        return effort::dispatch_settings(args);
    }
    if command == "effort" {
        return effort::dispatch(args);
    }
    if command == "search" {
        return search::dispatch(args);
    }
    if command == "schedules" {
        return Err("Native schedule management is available for DeepSeek sessions; other agents report schedules read-only".into());
    }
    if command == "send-now" { return input_queue::dispatch(args); }
    if command == "send" {
        return input::dispatch(args);
    }
    if command == "answer" {
        return questions::dispatch(args);
    }
    let dry = args.iter().any(|arg| arg == "--dry-run");
    let args: Vec<String> = args
        .iter()
        .filter(|arg| *arg != "--dry-run")
        .cloned()
        .collect();
    match command.as_str() {
        "terminate" => return terminate::dispatch(&args, dry),
        "inspect" => {
            let name = args
                .first()
                .ok_or("usage: hgs inspect <session> [--archive ID] [--after cursor] [--skip-processes]")?;
            let mut after = 0;
            let mut archive_id = None;
            let mut agent_id = None;
            let mut include_processes = true;
            let mut index = 1;
            while index < args.len() {
                if args[index] == "--skip-processes" { include_processes = false; index += 1; continue; }
                let value = args.get(index + 1).ok_or("missing inspect option value")?;
                match args[index].as_str() {
                    "--after" => {
                        after = value
                            .parse::<i64>()
                            .map_err(|_| "cursor must be nonnegative")?
                    }
                    "--archive" => archive_id = Some(value.as_str()),
                    "--agent" => agent_id = Some(value.as_str()),
                    _ => return Err("invalid inspect option".into()),
                }
                index += 2;
            }
            if after < 0 {
                return Err("cursor must be nonnegative".into());
            }
            println!(
                "{}",
                if let Some(id) = agent_id {
                    subagents::inspection(name, id, archive_id)?
                } else {
                    journal::inspection(name, after, archive_id, include_processes)?
                }
            );
        }
        "archive" => {
            if args.len() != 1 {
                return Err("usage: hgs archive <session>".into());
            }
            return archive::manual(&args[0], dry);
        }
        "pause" => return lifecycle::pause(&args, dry),
        "resume" => return lifecycle::resume(&args, dry),
        "fresh" => return lifecycle::fresh(&args, dry),
        "rename" => return rename::dispatch(&args, dry),
        "merge" => println!("{}", merge_snapshot(stdin_json()?)?),
        "list" => {
            let snapshot = live()?;
            let box_value = merge_snapshot(json!({"sessions": []}))?;
            for session in box_value["sessions"].as_array().unwrap() {
                if !snapshot.contains_key(string(session, "name"))
                    && string(session, "state") != "archived"
                {
                    println!(
                        "  {:40} {} (hgs resume)",
                        string(session, "name"),
                        string(session, "state")
                    );
                }
            }
        }
        "exists" => {
            archive::reconcile()?;
            let name = args.first().ok_or("missing session name")?;
            return Ok(if attempts::binding_exists(name)? {
                0
            } else {
                1
            });
        }
        "forget" => {
            if args.len() == 3 && args[1] == "--archive" {
                archive::forget(&args[0], &args[2], dry)?;
                return Ok(0);
            }
            if args.len() != 1 {
                return Err("usage: hgs forget <session> [--archive ID]".into());
            }
            if dry {
                println!("hgs: would forget {}", args[0]);
                return Ok(0);
            }
            let _guard = lock(None)?;
            if record_path(&args[0]).exists() {
                rename::remove_legacy(&read(&args[0])?)?;
            }
            match std::fs::remove_file(record_path(args.first().ok_or("missing session name")?)) {
                Ok(()) => {}
                Err(error) if error.kind() == io::ErrorKind::NotFound => {}
                Err(error) => return Err(error.to_string()),
            }
        }
        _ => return Err(format!("unknown state operation: {command}")),
    }
    Ok(0)
}

#[cfg(all(test, not(unix)))]
mod control_console_tests {
    use super::*;
    use crate::platform::LOCAL_SESSIONS_UNSUPPORTED as UNSUPPORTED;

    /// Set for the process `entry_points_answer_as_a_control_console` starts.
    const PROBE: &str = "HGS_TEST_CONTROL_CONSOLE";

    /// Run by `entry_points_answer_as_a_control_console` in a process whose
    /// profile is a new temporary folder: whether local sessions are enabled is
    /// the `local-sessions` marker in the profile (spec D2), and the user running
    /// these tests may have enabled them.
    #[test]
    #[ignore = "run by entry_points_answer_as_a_control_console"]
    fn control_console_probe() {
        if std::env::var_os(PROBE).is_none() {
            return;
        }
        assert_eq!(dispatch(&["list".into()]).unwrap_err(), UNSUPPORTED);
        assert_eq!(
            merge_snapshot(json!({"sessions":[]})).unwrap_err(),
            UNSUPPORTED
        );
        assert_eq!(account_context("claude/p").unwrap_err(), UNSUPPORTED);
        assert_eq!(startup_failure("claude/p").unwrap_err(), UNSUPPORTED);
        assert_eq!(recovery_worker_running(), None);
        assert_eq!(
            worktree_catalog(Path::new(r"C:\work")),
            json!({"state":"unavailable","detail":UNSUPPORTED,"stale":true,"worktrees":[]})
        );
        let error = with_launch_binding("claude/p", |_| Ok(())).unwrap_err();
        assert_eq!(error.to_string(), UNSUPPORTED);

        // The sessions of an isolated test host reach `dispatch` (their hooks)
        // without the marker: under the override, for an existing state
        // directory that HGS_STATE_DIR names and that is not the profile's
        // default one. Nothing else opens.
        use crate::host::gate::ALLOW_UNSTAGED;
        let hook = || dispatch(&["hook".into()]);
        for name in ["HGS_SESSION", "HGS_RUN_ID"] {
            std::env::remove_var(name);
        }
        let state = PathBuf::from(std::env::var_os("HGS_STATE_DIR").unwrap());
        std::env::set_var(ALLOW_UNSTAGED, "1");
        assert_eq!(hook().unwrap_err(), UNSUPPORTED);
        std::fs::create_dir_all(&state).unwrap();
        assert_eq!(hook(), Ok(0));
        assert_eq!(session_operations(), Ok(()));
        assert_eq!(local_sessions().unwrap_err(), UNSUPPORTED);
        assert_eq!(
            merge_snapshot(json!({"sessions":[]})).unwrap_err(),
            UNSUPPORTED
        );
        assert_eq!(startup_failure("claude/p").unwrap_err(), UNSUPPORTED);
        std::env::set_var(ALLOW_UNSTAGED, "true");
        assert_eq!(hook().unwrap_err(), UNSUPPORTED);
        std::env::set_var(ALLOW_UNSTAGED, "1");
        let profile = PathBuf::from(std::env::var_os("USERPROFILE").unwrap());
        let default = profile.join(r".local\state\hgs\sessions");
        std::fs::create_dir_all(&default).unwrap();
        std::env::set_var("HGS_STATE_DIR", &default);
        assert_eq!(hook().unwrap_err(), UNSUPPORTED);
        std::env::set_var("HGS_STATE_DIR", &state);
        std::env::remove_var(ALLOW_UNSTAGED);
        assert_eq!(hook().unwrap_err(), UNSUPPORTED);

        // The marker alone opens the entry points.
        let layout = crate::host::staging::Layout::current().unwrap();
        assert!(crate::host::staging::enable(&layout.marker).unwrap());
        assert_eq!(local_sessions(), Ok(()));
    }

    #[test]
    fn entry_points_answer_as_a_control_console() {
        let dir = tempfile::tempdir().unwrap();
        let output = std::process::Command::new(std::env::current_exe().unwrap())
            .args([
                "state::control_console_tests::control_console_probe",
                "--exact",
                "--ignored",
                "--test-threads=1",
            ])
            .env(PROBE, "1")
            .env("HOME", dir.path())
            .env("USERPROFILE", dir.path())
            .env("HGS_STATE_DIR", dir.path().join("state"))
            .env_remove("HGS_CONFIG_DIR")
            .env_remove("XDG_STATE_HOME")
            .env_remove(crate::host::gate::ALLOW_UNSTAGED)
            .stdin(std::process::Stdio::null())
            .output()
            .unwrap();
        assert!(
            output.status.success(),
            "{}{}",
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        );
        // The probe ran, rather than matching no test.
        assert!(
            String::from_utf8_lossy(&output.stdout).contains("1 passed"),
            "{}",
            String::from_utf8_lossy(&output.stdout)
        );
    }
}
