//! `hgs task`: the project task tracker on this computer. Tasks are Loro
//! documents kept by `zerus-replica` below `<state dir>/replica`; every
//! command prints JSON. See docs/superpowers/specs/2026-10-13-project-task-queue-design.md
//! and docs/superpowers/specs/2026-10-18-task-replication-loro-design.md.
//!
//! Lock order: catalog lookups (project, machine identity) happen before the
//! project store is opened, and stdin is read before it too; the hgs state lock
//! (taken when pasting) is only ever taken while holding the project lock.
use crate::cli::{Error, Result};
use crate::config::Config;
use serde_json::{json, Value};
use std::collections::BTreeMap;
use std::io::Read;
use std::time::{SystemTime, UNIX_EPOCH};
use zerus_replica::{
    cancel_message, task_prompt, Actor, NewTask, Position, ProjectStore, SessionKey, Settings,
    Stage, Target, TaskDoc, Tools, MAX_TEXT,
};

const USAGE: &str = "usage: hgs task list --project P [--stage S] | get REF | add --project P [--backlog] [--target SESSION|any [--host ALIAS]] [--position N] | edit REF | move REF --top|--before REF|--after REF | stage REF backlog|todo|done | target REF SESSION|any [--host ALIAS] | send REF [--session SESSION [--host ALIAS]] | cancel REF [--reason TEXT] | report REF | comment REF | settings --project P [--set]
  REF is T-xxxxxx with --project P, or PROJECT/T-xxxxxx; texts and --set settings are read from stdin";

fn fail(message: impl std::fmt::Display) -> Error {
    Error::new(1, message.to_string())
}

fn now() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_millis() as i64)
}

fn stdin_text(what: &str) -> Result<String> {
    let mut bytes = Vec::new();
    std::io::stdin()
        .take(MAX_TEXT as u64 + 1)
        .read_to_end(&mut bytes)
        .map_err(fail)?;
    if bytes.len() > MAX_TEXT {
        return Err(fail(format!("the {what} is longer than 64 KiB")));
    }
    String::from_utf8(bytes).map_err(|_| fail(format!("the {what} is not UTF-8")))
}

fn option<'a>(args: &'a [String], name: &str) -> Result<Option<&'a str>> {
    match args.iter().position(|a| a == name) {
        Some(i) => args
            .get(i + 1)
            .map(|v| Some(v.as_str()))
            .ok_or_else(|| fail(format!("{name} needs a value"))),
        None => Ok(None),
    }
}

fn flag(args: &[String], name: &str) -> bool {
    args.iter().any(|a| a == name)
}

/// The project's catalog ID and current name, by ID or name.
fn project(config: &Config, wanted: &str) -> Result<(String, String)> {
    let projects = crate::swarm::projects(config).map_err(fail)?;
    if let Some(found) = projects.iter().find(|(id, _)| id == wanted) {
        return Ok(found.clone());
    }
    let named: Vec<_> = projects.iter().filter(|(_, name)| name == wanted).collect();
    match named.as_slice() {
        [one] => Ok((*one).clone()),
        [] => Err(fail(format!("no project {wanted:?} in the catalog"))),
        _ => Err(fail(format!(
            "{wanted:?} names more than one project; use its ID"
        ))),
    }
}

/// A task reference: `PROJECT/T-xxxxxx`, or `T-xxxxxx` (or a UUID) with `--project`.
struct Reference {
    project: (String, String),
    task: String,
}

fn reference(config: &Config, args: &[String]) -> Result<Reference> {
    let value = args.first().ok_or_else(|| fail(USAGE))?;
    let (project_name, task) = match value.rsplit_once('/') {
        Some((project, task)) => (Some(project), task),
        None => (option(args, "--project")?, value.as_str()),
    };
    let project_name =
        project_name.ok_or_else(|| fail("name the project: PROJECT/T-xxxxxx or --project P"))?;
    Ok(Reference {
        project: project(config, project_name)?,
        task: task.to_owned(),
    })
}

fn store(config: &Config, project_id: &str) -> Result<ProjectStore> {
    ProjectStore::open(&config.state.join("replica"), project_id).map_err(fail)
}

/// This computer and the swarm members it verified itself, by SSH alias.
struct Machines {
    here: String,
    peers: BTreeMap<String, String>,
}

fn machines(config: &Config) -> Result<Machines> {
    let here = crate::swarm::local_node_id(config).map_err(fail)?;
    #[cfg(unix)]
    let peers = crate::swarm::verified_bindings(config, &here);
    #[cfg(not(unix))]
    let peers = BTreeMap::new();
    Ok(Machines { here, peers })
}

impl Machines {
    /// The computer `--host ALIAS` names, or this one without it.
    fn node(&self, host: Option<&str>) -> Result<String> {
        match host.filter(|h| !h.is_empty()) {
            None => Ok(self.here.clone()),
            Some(alias) => self.peers.get(alias).cloned().ok_or_else(|| {
                fail(format!("{alias} is not a verified computer of this swarm"))
            }),
        }
    }

    /// The alias of a verified member by its node.
    fn alias(&self, node: &str) -> Option<String> {
        self.peers.iter().find(|(_, id)| id.as_str() == node).map(|(alias, _)| alias.clone())
    }

    /// How callers name a computer: "" for this one, a verified member's
    /// alias, or null when this computer does not know it.
    fn host(&self, node: &str) -> Value {
        if node == self.here {
            return json!("");
        }
        self.peers
            .iter()
            .find(|(_, id)| id.as_str() == node)
            .map_or(Value::Null, |(alias, _)| json!(alias))
    }
}

/// The agent of the session that runs this command, as hooks know it (both
/// `HGS_SESSION` and `HGS_RUN_ID`), or the person. A send or cancel another
/// computer forwards (`forwardable`) names its agent with `--agent NODE/SESSION`;
/// an agent itself always acts as its own session.
fn actor(here: &str, args: &[String], forwardable: bool) -> Result<Actor> {
    let var = |name| std::env::var(name).ok().filter(|v| !v.is_empty());
    if let Some(agent) = option(args, "--agent")? {
        if !forwardable {
            return Err(fail("only send and cancel take --agent"));
        }
        if var("HGS_SESSION").is_some() && var("HGS_RUN_ID").is_some() {
            return Err(fail("--agent is for commands another computer forwards; an agent acts as its own session"));
        }
        let (machine, session) = agent
            .split_once('/')
            .filter(|(machine, session)| uuid::Uuid::parse_str(machine).is_ok() && !session.is_empty())
            .ok_or_else(|| fail("--agent is NODE/SESSION, NODE being a computer's identity"))?;
        return Ok(Actor::Agent(SessionKey {
            machine: machine.to_owned(),
            session: session.to_owned(),
        }));
    }
    Ok(match (var("HGS_SESSION"), var("HGS_RUN_ID")) {
        (Some(session), Some(_)) => Actor::Agent(SessionKey {
            machine: here.to_owned(),
            session,
        }),
        _ => Actor::Person,
    })
}

fn view(task: &TaskDoc, project_name: &str, with_log: bool, machines: &Machines) -> Result<Value> {
    let mut value = serde_json::to_value(task.view().map_err(fail)?).map_err(fail)?;
    value["ref"] = json!(format!("{project_name}/{}", task.id()));
    // Every session reference names its computer: the task's target, session and
    // author, and each history entry's session and actor.
    let name = |reference: &mut Value| {
        if let Some(machine) = reference["machine"].as_str().map(str::to_owned) {
            reference["host"] = machines.host(&machine);
        }
    };
    for key in ["target", "session", "author"] {
        name(&mut value[key]);
    }
    if let Some(log) = value["log"].as_array_mut() {
        for entry in log {
            name(&mut entry["session"]);
            name(&mut entry["actor"]);
        }
    }
    if !with_log {
        value.as_object_mut().expect("object").remove("log");
    }
    Ok(value)
}

fn print(value: Value) -> Result<i32> {
    println!("{value}");
    Ok(0)
}

/// Refuses a prompt that the task prompt could not carry when it is sent.
fn check_prompt(store: &ProjectStore, task: &TaskDoc, project_name: &str) -> Result<()> {
    let settings = store.project().map_err(fail)?.settings();
    let view = task.view().map_err(fail)?;
    task_prompt(&settings.task_template, &view, project_name, Tools::Cli)
        .map(|_| ())
        .map_err(fail)
}

pub fn dispatch(config: &Config, args: &[String], dry: bool) -> Result<i32> {
    if dry {
        return Err(fail("task commands do not support --dry-run"));
    }
    let Some((action, args)) = args.split_first() else {
        return Err(fail(USAGE));
    };
    match action.as_str() {
        "list" => list(config, args),
        "get" => {
            let r = reference(config, args)?;
            let machines = machines(config)?;
            let task = store(config, &r.project.0)?
                .find(&r.task)
                .map_err(fail)?;
            print(json!({"project": {"id": r.project.0, "name": r.project.1}, "task": view(&task, &r.project.1, true, &machines)?}))
        }
        "add" => add(config, args),
        "edit" | "move" | "stage" | "target" | "comment" | "report" | "send" | "cancel" => {
            change(config, action, args)
        }
        "settings" => settings(config, args),
        _ => Err(fail(USAGE)),
    }
}

fn list(config: &Config, args: &[String]) -> Result<i32> {
    let (id, name) = project(
        config,
        option(args, "--project")?.ok_or_else(|| fail(USAGE))?,
    )?;
    let only = option(args, "--stage")?
        .map(Stage::parse)
        .transpose()
        .map_err(fail)?;
    let machines = machines(config)?;
    // One load of the project; each stage is then a filter of the sorted list.
    let (tasks, damaged) = store(config, &id)?.listing().map_err(fail)?;
    let mut out = Vec::new();
    for stage in [
        Stage::Review,
        Stage::Active,
        Stage::Todo,
        Stage::Backlog,
        Stage::Done,
    ] {
        if only.is_some_and(|s| s != stage) {
            continue;
        }
        for task in tasks.iter().filter(|t| t.stage().ok() == Some(stage)) {
            out.push(view(task, &name, false, &machines)?);
        }
    }
    print(json!({"project": {"id": id, "name": name}, "tasks": out, "damaged": damaged}))
}

fn add(config: &Config, args: &[String]) -> Result<i32> {
    let text = stdin_text("prompt")?;
    let (id, name) = project(
        config,
        option(args, "--project")?.ok_or_else(|| fail(USAGE))?,
    )?;
    let machines = machines(config)?;
    let author = actor(&machines.here, args, false)?;
    let stage = if flag(args, "--backlog") {
        Stage::Backlog
    } else {
        Stage::Todo
    };
    // A task an agent files is always pinned to that agent's session.
    if option(args, "--host")?.is_some() && matches!(option(args, "--target")?, None | Some("any")) {
        return Err(fail("--host needs a --target session"));
    }
    let target = match (&author, option(args, "--target")?) {
        (Actor::Agent(_), Some(_)) => {
            return Err(fail(
                "a task an agent files is pinned to its own session; --target is for people",
            ))
        }
        (Actor::Agent(own), None) => Target::Session(own.clone()),
        (Actor::Person, None | Some("any")) => Target::Any,
        (Actor::Person, Some(session)) => Target::Session(SessionKey {
            machine: machines.node(option(args, "--host")?)?,
            session: session.to_owned(),
        }),
    };
    let position = match option(args, "--position")? {
        Some(p) => Position::Index(
            p.parse::<usize>()
                .ok()
                .filter(|p| *p >= 1)
                .ok_or_else(|| fail("--position is a number from 1"))?
                - 1,
        ),
        None => Position::End,
    };
    let store = store(config, &id)?;
    let rank = store.rank_for(stage, &position, None).map_err(fail)?;
    let short = store.unused_short_id().map_err(fail)?;
    let mut task = TaskDoc::create(NewTask {
        project: &id,
        id: &short,
        text: &text,
        stage,
        target,
        author,
        rank: &rank,
        now: now(),
    })
    .map_err(fail)?;
    check_prompt(&store, &task, &name)?;
    store.save(&mut task).map_err(fail)?;
    print(json!({"task": view(&task, &name, true, &machines)?}))
}

fn change(config: &Config, action: &str, args: &[String]) -> Result<i32> {
    let text = match action {
        "edit" => Some(stdin_text("prompt")?),
        "comment" => Some(stdin_text("comment")?),
        "report" => Some(stdin_text("report")?),
        _ => None,
    };
    let r = reference(config, args)?;
    let (project_id, project_name) = (&r.project.0, &r.project.1);
    let machines = machines(config)?;
    let here = machines.here.as_str();
    let who = actor(here, args, matches!(action, "send" | "cancel"))?;
    let store = store(config, project_id)?;
    let mut task = store.find(&r.task).map_err(fail)?;
    // A send or cancel for a session another computer runs happens there.
    let session = match action {
        "send" => Some(send_session(&task, args, &who, &machines)?),
        "cancel" => task.view().map_err(fail)?.session.map(|session| (session, false)),
        _ => None,
    };
    if let Some((session, _)) = session.as_ref().filter(|(session, _)| session.machine != here) {
        let uuid = task.uuid().to_owned();
        drop(store);
        return forwarded(config, &machines, &r.project, &uuid, action, args, session, &who);
    }
    let text = text.unwrap_or_default();
    let mut extra = json!({});
    match action {
        "edit" => {
            task.edit_text(&text, &who, now()).map_err(fail)?;
            check_prompt(&store, &task, project_name)?;
        }
        "comment" => task.comment(&text, &who, now()).map_err(fail)?,
        "report" => {
            let moved = task.record_report(&text, &who, now()).map_err(fail)?;
            extra = json!({"moved_to_review": moved});
        }
        "move" => {
            let position = if flag(args, "--top") {
                Position::Top
            } else if let Some(anchor) = option(args, "--before")? {
                Position::Before(anchor.rsplit('/').next().unwrap_or(anchor).to_owned())
            } else if let Some(anchor) = option(args, "--after")? {
                Position::After(anchor.rsplit('/').next().unwrap_or(anchor).to_owned())
            } else {
                return Err(fail(USAGE));
            };
            let stage = task.stage().map_err(fail)?;
            let rank = store
                .rank_for(stage, &position, Some(task.uuid()))
                .map_err(fail)?;
            task.set_rank(&rank).map_err(fail)?;
        }
        "stage" => {
            let to = Stage::parse(args.get(1).ok_or_else(|| fail(USAGE))?).map_err(fail)?;
            let from = task.stage().map_err(fail)?;
            task.set_stage(to, &who, now()).map_err(fail)?;
            // A task that changes list goes to the top of its new list.
            if from != to && matches!(to, Stage::Todo | Stage::Backlog) {
                let rank = store
                    .rank_for(to, &Position::Top, Some(task.uuid()))
                    .map_err(fail)?;
                task.set_rank(&rank).map_err(fail)?;
            }
        }
        "target" => {
            let value = args.get(1).ok_or_else(|| fail(USAGE))?;
            if value == "any" && option(args, "--host")?.is_some() {
                return Err(fail("--host needs a --target session"));
            }
            let target = if value == "any" {
                Target::Any
            } else {
                Target::Session(SessionKey {
                    machine: machines.node(option(args, "--host")?)?,
                    session: value.clone(),
                })
            };
            task.set_target(&target, &who, now()).map_err(fail)?;
        }
        "send" => {
            let (session, pin) = session.expect("a send has its session");
            extra = send(&store, &mut task, project_name, session, pin, &who)?;
        }
        "cancel" => extra = cancel(&store, &mut task, project_name, args, &who)?,
        _ => return Err(fail(USAGE)),
    }
    store.save(&mut task).map_err(fail)?;
    let mut out = json!({"task": view(&task, project_name, true, &machines)?});
    for (key, value) in extra.as_object().expect("object") {
        out[key] = value.clone();
    }
    print(out)
}

/// The session a send goes to, and whether it pins the task there: --session
/// (with --host on another computer; an agent's choice also pins the task),
/// the pinned session, or for an Any task the agent's own session.
fn send_session(task: &TaskDoc, args: &[String], who: &Actor, machines: &Machines) -> Result<(SessionKey, bool)> {
    let view = task.view().map_err(fail)?;
    let host = option(args, "--host")?;
    let named = |session: &str| -> Result<SessionKey> {
        Ok(SessionKey {
            machine: machines.node(host)?,
            session: session.to_owned(),
        })
    };
    match (option(args, "--session")?, &view.target, who) {
        (None, ..) if host.is_some() => Err(fail("--host needs a --session")),
        (Some(name), Target::Session(pinned), Actor::Person) if *pinned != named(name)? => Err(fail(format!(
            "{} is pinned to {}; change its session first",
            task.id(),
            pinned.session
        ))),
        (Some(name), _, Actor::Person) => Ok((named(name)?, false)),
        (Some(name), _, Actor::Agent(_)) => Ok((named(name)?, true)),
        (None, Target::Session(pinned), _) => Ok((pinned.clone(), false)),
        (None, Target::Any, Actor::Agent(own)) => Ok((own.clone(), false)),
        (None, Target::Any, Actor::Person) => Err(fail("choose the session with --session")),
    }
}

/// Runs a send or cancel on the computer that runs `session`. That computer
/// gets this project's tasks first, so it checks and records the action in its
/// own copy and pastes in its session; its copy then comes back. Nothing
/// happens when it cannot be reached.
#[allow(clippy::too_many_arguments)]
fn forwarded(
    config: &Config,
    machines: &Machines,
    project: &(String, String),
    uuid: &str,
    action: &str,
    args: &[String],
    session: &SessionKey,
    who: &Actor,
) -> Result<i32> {
    let alias = machines
        .alias(&session.machine)
        .ok_or_else(|| fail(format!("{} is on a computer this one has not verified", session.session)))?;
    crate::swarm::replicate(config, &alias, &project.0).map_err(|e| fail(format!("{e:#}")))?;
    let mut remote: Vec<String> = vec!["task".into(), action.into(), uuid.into(), "--project".into(), project.0.clone()];
    if action == "send" && option(args, "--session")?.is_some() {
        remote.extend(["--session".into(), session.session.clone()]);
    }
    if let Some(reason) = option(args, "--reason")?.filter(|_| action == "cancel") {
        remote.extend(["--reason".into(), reason.into()]);
    }
    if let Actor::Agent(agent) = who {
        remote.extend(["--agent".into(), format!("{}/{}", agent.machine, agent.session)]);
    }
    let reply = crate::swarm::remote_hgs(config, &alias, &remote).map_err(|e| {
        fail(format!("{e:#}").replacen(&format!("{alias}: hgs: "), &format!("{alias}: "), 1))
    })?;
    // Its copy comes back now (its identity was checked a moment ago); when
    // that fails the next sync brings it.
    if let Err(error) = crate::swarm::exchange_tasks(config, &alias, &project.0) {
        eprintln!("hgs: {error:#}");
    }
    let task = store(config, &project.0)?.find(uuid).map_err(fail)?;
    let mut out = json!({"task": view(&task, &project.1, true, machines)?});
    for key in ["receipt", "cancel_message"] {
        if let Some(value) = reply.get(key) {
            out[key] = value.clone();
        }
    }
    print(out)
}

/// Pastes the task prompt into its session on this computer.
fn send(
    store: &ProjectStore,
    task: &mut TaskDoc,
    project_name: &str,
    session: SessionKey,
    pin: bool,
    who: &Actor,
) -> Result<Value> {
    // Everything that could stop the send from being recorded is checked
    // before the paste; only I/O can fail after it.
    task.check_send().map_err(fail)?;
    let view = task.view().map_err(fail)?;
    let settings = store.project().map_err(fail)?.settings();
    let starts = store
        .agent_starts_since(now() - 3_600_000)
        .map_err(fail)?;
    settings
        .check_agent(who, &session, true, starts)
        .map_err(fail)?;
    if pin && view.target != Target::Session(session.clone()) {
        task.set_target(&Target::Session(session.clone()), who, now())
            .map_err(fail)?;
    }
    let prompt = task_prompt(
        &settings.task_template,
        &task.view().map_err(fail)?,
        project_name,
        Tools::Cli,
    )
    .map_err(fail)?;
    let receipt = crate::state::submit_task_text(&session.session, &prompt).map_err(fail)?;
    let request_id = receipt["request_id"].as_str().unwrap_or_default().to_owned();
    task.record_send(&session, &request_id, who, now())
        .map_err(|e| {
            fail(format!(
                "the prompt was sent, but the task could not record it: {e}"
            ))
        })?;
    Ok(json!({"receipt": receipt}))
}

/// Moves the task to In review and pastes the cancel message; the turn is not stopped.
fn cancel(
    store: &ProjectStore,
    task: &mut TaskDoc,
    project_name: &str,
    args: &[String],
    who: &Actor,
) -> Result<Value> {
    let session = task
        .view()
        .map_err(fail)?
        .session
        .ok_or_else(|| fail("only a task In progress can be cancelled"))?;
    if let Actor::Agent(own) = who {
        if *own == session {
            return Err(fail(format!(
                "You cannot cancel {project_name}/{}, a task you were sent. If it cannot be done, report what is left undone.",
                task.id()
            )));
        }
    }
    let settings = store.project().map_err(fail)?.settings();
    settings
        .check_agent(who, &session, false, 0)
        .map_err(fail)?;
    let reason = option(args, "--reason")?.unwrap_or_default();
    task.record_cancel(reason, who, now()).map_err(fail)?;
    store.save(task).map_err(fail)?;
    let message = cancel_message(
        &settings.cancel_template,
        &task.view().map_err(fail)?,
        project_name,
        Tools::Cli,
    )
    .map_err(fail)?;
    let pasted = crate::state::submit_task_text(&session.session, &message).map_err(|e| e.to_string());
    Ok(match pasted {
        Ok(receipt) => json!({"cancel_message": {"sent": true, "receipt": receipt}}),
        Err(error) => {
            task.note(&format!("Cancel message not sent: {error}"), who, now())
                .map_err(fail)?;
            json!({"cancel_message": {"sent": false, "error": error}})
        }
    })
}

fn settings(config: &Config, args: &[String]) -> Result<i32> {
    let invalid = |e: &dyn std::fmt::Display| fail(format!("invalid settings: {e}"));
    let changes = if flag(args, "--set") {
        match serde_json::from_str(&stdin_text("settings")?).map_err(|e| invalid(&e))? {
            Value::Object(changes) => Some(changes),
            _ => return Err(invalid(&"expected a JSON object")),
        }
    } else {
        None
    };
    let (id, name) = project(
        config,
        option(args, "--project")?.ok_or_else(|| fail(USAGE))?,
    )?;
    let store = store(config, &id)?;
    let mut doc = store.project().map_err(fail)?;
    if let Some(changes) = changes {
        // Only the fields given change; the others keep their current values.
        let mut merged = serde_json::to_value(doc.settings()).map_err(fail)?;
        for (key, value) in changes {
            if merged.get(&key).is_none() {
                return Err(invalid(&format!("unknown field {key:?}")));
            }
            merged[key] = value;
        }
        let wanted: Settings = serde_json::from_value(merged).map_err(|e| invalid(&e))?;
        doc.set_settings(&wanted).map_err(fail)?;
        store.save_project(&doc).map_err(fail)?;
    }
    print(json!({"project": {"id": id, "name": name}, "settings": doc.settings()}))
}
