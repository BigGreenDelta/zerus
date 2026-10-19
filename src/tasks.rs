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

mod answers;

const USAGE: &str = "usage: hgs task list --project P [--stage S] | get REF | add --project P [--backlog] [--target SESSION|any [--host ALIAS]] [--position N] | edit REF | move REF --top|--before REF|--after REF|--position N | stage REF backlog|todo|done | target REF SESSION|any [--host ALIAS] | send REF [--session SESSION [--host ALIAS]] | cancel REF [--reason TEXT] | report REF | comment REF [--json] | settings --project P [--set]
  REF is T-xxxxxx with --project P, or PROJECT/T-xxxxxx; texts and --set settings are read from stdin";

/// What Claude Code and Codex sessions read about the project's tasks each time
/// their SessionStart hooks run (spec "Instructions at session start").
const AGENT_INSTRUCTIONS: &str = concat!(
    "Zerus tasks are this project's shared task list. Run `hgs task` in your shell: it acts as your session, on your session's project; `hgs guide tasks` lists the commands. Texts go on stdin, for example hgs task report <ref> <<'EOF' ... EOF.\n\n",
    "Filing. When the person says \"поставь таск\", \"добавь задачу\", \"запиши задачу\", \"в очередь\", \"на потом\", \"file a task\", \"add to the queue\" or \"for later\", file it with `hgs task add`. Never put these into TodoWrite or your own plan. New tasks go to the end of To do unless the person names a place (--backlog, --position N).\n\n",
    "Working. A message starting \"[Zerus task <ref>]\" is a task for you; do it like any request from the person. When the prompt says the task was sent before, read its history with `hgs task get <ref>` before you start. If you need the person, ask in this conversation as usual. When the work is done, or you find that it cannot be done, report with `hgs task report <ref>`: what you changed, how you checked it, and what is left undone or why it cannot be done.\n\n",
    "Cancelled. \"[Zerus task <ref> cancelled]\" means stop that task now and follow the message.\n\n",
    "Managing. `hgs task list` and `hgs task get` show tasks and their history; move, stage, target, edit, send and cancel change them as the person can. Accept a task (`hgs task stage <ref> done`) only when the person asks you to.\n\n",
    "Comments. `hgs task comment <ref>` adds a note for people and agents; it is never pasted into a session."
);

/// The SessionStart hook output that gives a session of `agent` the task
/// instructions; other agents get them once their hooks are reviewed.
pub(crate) fn session_context(agent: &str) -> Option<Value> {
    matches!(agent, "claude" | "codex").then(|| {
        json!({"hookSpecificOutput": {"hookEventName": "SessionStart", "additionalContext": AGENT_INSTRUCTIONS}})
    })
}

fn fail(message: impl std::fmt::Display) -> Error {
    Error::new(1, message.to_string())
}

fn now() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_millis() as i64)
}

fn stdin_text(what: &str, example: &str) -> Result<String> {
    let mut bytes = Vec::new();
    std::io::stdin()
        .take(MAX_TEXT as u64 + 1)
        .read_to_end(&mut bytes)
        .map_err(fail)?;
    if bytes.len() > MAX_TEXT {
        return Err(fail(format!("the {what} is longer than 64 KiB")));
    }
    let text = String::from_utf8(bytes).map_err(|_| fail(format!("the {what} is not UTF-8")))?;
    // Agents tend to pass texts as arguments; say where they go.
    if text.trim().is_empty() && !example.is_empty() {
        return Err(fail(format!(
            "the {what} is empty; give it on stdin, for example: hgs task {example} <<'EOF'"
        )));
    }
    Ok(text)
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

/// The project a command works on when it names none: the calling agent's
/// session's catalog project. The person is told `missing`.
fn own_project(config: &Config, who: &Actor, missing: &str) -> Result<(String, String)> {
    let Actor::Agent(own) = who else {
        return Err(fail(missing));
    };
    crate::swarm::session_project(config, &own.session)
        .map_err(fail)?
        .ok_or_else(|| {
            fail(format!(
                "{} belongs to no project; name one: PROJECT/T-xxxxxx or --project P",
                own.session
            ))
        })
}

/// `--project P`, or the calling agent's own project.
fn project_arg(config: &Config, args: &[String], who: &Actor) -> Result<(String, String)> {
    match option(args, "--project")? {
        Some(wanted) => project(config, wanted),
        None => own_project(config, who, USAGE),
    }
}

/// A task reference: `PROJECT/T-xxxxxx`, or `T-xxxxxx` (or a UUID) with
/// `--project`, or of the calling agent's own project.
struct Reference {
    project: (String, String),
    task: String,
}

fn reference(config: &Config, args: &[String], who: &Actor) -> Result<Reference> {
    let value = args.first().ok_or_else(|| fail(USAGE))?;
    let (project_name, task) = match value.rsplit_once('/') {
        Some((project, task)) => (Some(project), task),
        None => (option(args, "--project")?, value.as_str()),
    };
    let found = match project_name {
        Some(name) => project(config, name)?,
        None => own_project(config, who, "name the project: PROJECT/T-xxxxxx or --project P")?,
    };
    Ok(Reference {
        project: found,
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

/// The agent of the session that runs this command, or the person. A command
/// whose environment names a session (`HGS_SESSION`, `HGS_RUN_ID`) acts as its
/// agent only when it proves it as `hgs self` does: the run is live on this
/// computer and the command descends from its agent; the proof follows a rename
/// to the session's current name. A send or cancel another computer forwards
/// (`forwardable`) names its agent with `--agent NODE/SESSION`.
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
    let (Some(claimed), Some(_)) = (var("HGS_SESSION"), var("HGS_RUN_ID")) else {
        return Ok(Actor::Person);
    };
    let record = crate::state::session_graph::caller_record().ok_or_else(|| {
        fail(format!(
            "this command names the agent session {claimed}, but that run is not live on this computer or does not run it; run hgs task from the session's own shell"
        ))
    })?;
    Ok(Actor::Agent(SessionKey {
        machine: here.to_owned(),
        session: record["name"].as_str().unwrap_or(&claimed).to_owned(),
    }))
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

/// The place of `task` in its list, counting from 1.
fn place(store: &ProjectStore, task: &TaskDoc) -> Result<usize> {
    let stage = task.stage().map_err(fail)?;
    let (tasks, _) = store.listing().map_err(fail)?;
    Ok(tasks
        .iter()
        .filter(|t| t.stage().ok() == Some(stage))
        .position(|t| t.uuid() == task.uuid())
        .map_or(0, |i| i + 1))
}

/// `out` with the line an agent reads (`answer`), for a change an agent made.
fn with_answer(who: &Actor, action: &str, mut out: Value, place: usize, destination: &str) -> Value {
    if let Actor::Agent(own) = who {
        out["answer"] = json!(answers::answer(action, &out, place, &own.session, destination));
    }
    out
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
    let out = execute(config, args)?;
    // An agent reads one line saying what happened; --json prints the task.
    match out.get("answer").and_then(Value::as_str) {
        Some(line) if !flag(args, "--json") => println!("{line}"),
        _ => println!("{out}"),
    }
    Ok(0)
}

/// Runs one task command and returns its JSON.
fn execute(config: &Config, args: &[String]) -> Result<Value> {
    let Some((action, args)) = args.split_first() else {
        return Err(fail(USAGE));
    };
    match action.as_str() {
        "list" => list(config, args),
        "get" => {
            let machines = machines(config)?;
            let who = actor(&machines.here, args, false)?;
            let r = reference(config, args, &who)?;
            let task = store(config, &r.project.0)?
                .find(&r.task)
                .map_err(fail)?;
            Ok(json!({"project": {"id": r.project.0, "name": r.project.1}, "task": view(&task, &r.project.1, true, &machines)?}))
        }
        "add" => add(config, args),
        "edit" | "move" | "stage" | "target" | "comment" | "report" | "send" | "cancel" => {
            change(config, action, args)
        }
        "settings" => settings(config, args),
        _ => Err(fail(USAGE)),
    }
}

fn list(config: &Config, args: &[String]) -> Result<Value> {
    let machines = machines(config)?;
    let who = actor(&machines.here, args, false)?;
    let (id, name) = project_arg(config, args, &who)?;
    let only = option(args, "--stage")?
        .map(Stage::parse)
        .transpose()
        .map_err(fail)?;
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
    Ok(json!({"project": {"id": id, "name": name}, "tasks": out, "damaged": damaged}))
}

/// `--position N`, counting from 1, as a list index.
fn position_index(args: &[String]) -> Result<Option<usize>> {
    option(args, "--position")?
        .map(|p| {
            p.parse::<usize>()
                .ok()
                .filter(|p| *p >= 1)
                .map(|p| p - 1)
                .ok_or_else(|| fail("--position is a number from 1"))
        })
        .transpose()
}

fn add(config: &Config, args: &[String]) -> Result<Value> {
    let text = stdin_text("prompt", "add")?;
    let machines = machines(config)?;
    let author = actor(&machines.here, args, false)?;
    let (id, name) = project_arg(config, args, &author)?;
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
    let position = position_index(args)?.map_or(Position::End, Position::Index);
    let store = store(config, &id)?;
    let rank = store.rank_for(stage, &position, None).map_err(fail)?;
    let short = store.unused_short_id().map_err(fail)?;
    let mut task = TaskDoc::create(NewTask {
        project: &id,
        id: &short,
        text: &text,
        stage,
        target,
        author: author.clone(),
        rank: &rank,
        now: now(),
    })
    .map_err(fail)?;
    check_prompt(&store, &task, &name)?;
    store.save(&mut task).map_err(fail)?;
    let at = place(&store, &task)?;
    Ok(with_answer(&author, "add", json!({"task": view(&task, &name, true, &machines)?}), at, ""))
}

fn change(config: &Config, action: &str, args: &[String]) -> Result<Value> {
    let text = match action {
        "edit" => Some(stdin_text("prompt", "edit REF")?),
        "comment" => Some(stdin_text("comment", "comment REF")?),
        "report" => Some(stdin_text("report", "report REF")?),
        _ => None,
    };
    let machines = machines(config)?;
    let here = machines.here.as_str();
    let who = actor(here, args, matches!(action, "send" | "cancel"))?;
    let r = reference(config, args, &who)?;
    let (project_id, project_name) = (&r.project.0, &r.project.1);
    // An agent reads a refused send as "SESSION cannot take it now: …"; a paste
    // that happened is never reworded.
    let refuse = |error: Error, to: &str| -> Error {
        if matches!(who, Actor::Agent(_))
            && action == "send"
            && !error.message.starts_with("the prompt was sent")
        {
            fail(answers::refused(to, &error.message))
        } else {
            error
        }
    };
    let store = store(config, project_id)?;
    let mut task = store.find(&r.task).map_err(fail)?;
    // A send or cancel for a session another computer runs happens there.
    let session = match action {
        "send" => Some(send_session(&task, args, &who, &machines)?),
        // Only a task In progress is cancelled; this computer refuses anything else.
        "cancel" if task.stage().map_err(fail)? == Stage::Active => {
            task.view().map_err(fail)?.session.map(|session| (session, false))
        }
        _ => None,
    };
    if let Some((session, _)) = session.as_ref().filter(|(session, _)| session.machine != here) {
        let to = session.session.clone();
        // This computer's copy refuses first: a task it shows In progress, or
        // in a stage that is not sent, never reaches the other computer.
        if action == "send" {
            task.check_send().map_err(|e| refuse(fail(e), &to))?;
        }
        let uuid = task.uuid().to_owned();
        drop(store);
        let out = match reach(config, &machines, project_id, session) {
            Ok(alias) => forwarded(config, &machines, &r.project, &uuid, action, args, session, &alias, &who),
            // A cancel goes through while that computer is out of reach: the task
            // moves to In review here and its session is not told.
            Err(reason) if action == "cancel" => cancel_here(config, &machines, &r.project, &uuid, args, &who, reason),
            Err(reason) => Err(fail(reason)),
        }
        .map_err(|e| refuse(e, &to))?;
        return Ok(with_answer(&who, action, out, 0, &to));
    }
    let text = text.unwrap_or_default();
    let mut extra = json!({});
    let mut destination = String::new();
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
            } else if let Some(index) = position_index(args)? {
                Position::Index(index)
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
            destination = session.session.clone();
            extra = send(&store, &mut task, project_name, session, pin, &who)
                .map_err(|e| refuse(e, &destination))?;
        }
        "cancel" => extra = cancel(&store, &mut task, project_name, args, &who, None)?,
        _ => return Err(fail(USAGE)),
    }
    store.save(&mut task).map_err(fail)?;
    let at = if action == "move" { place(&store, &task)? } else { 0 };
    let mut out = json!({"task": view(&task, project_name, true, &machines)?});
    for (key, value) in extra.as_object().expect("object") {
        out[key] = value.clone();
    }
    Ok(with_answer(&who, action, out, at, &destination))
}

/// The session a send goes to, and whether it pins the task there: --session
/// (with --host on another computer; an agent's choice also pins the task),
/// the pinned session, or for an Any task the agent's own session.
fn send_session(task: &TaskDoc, args: &[String], who: &Actor, machines: &Machines) -> Result<(SessionKey, bool)> {
    let view = task.view().map_err(fail)?;
    let host = option(args, "--host")?.filter(|host| !host.is_empty());
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

/// The alias of the computer that runs `session`, with this project's tasks
/// exchanged with it, or why it cannot be reached now.
fn reach(config: &Config, machines: &Machines, project: &str, session: &SessionKey) -> std::result::Result<String, String> {
    let alias = machines
        .alias(&session.machine)
        .ok_or_else(|| format!("{} is on a computer this one has not verified", session.session))?;
    crate::swarm::replicate(config, &alias, project).map_err(|e| format!("{e:#}"))?;
    Ok(alias)
}

/// Runs a send or cancel on the computer behind `alias`, which runs `session`
/// and now holds this project's tasks: it checks and records the action in its
/// own copy and pastes in its session; its copy then comes back.
#[allow(clippy::too_many_arguments)]
fn forwarded(
    config: &Config,
    machines: &Machines,
    project: &(String, String),
    uuid: &str,
    action: &str,
    args: &[String],
    session: &SessionKey,
    alias: &str,
    who: &Actor,
) -> Result<Value> {
    // That computer answers in JSON even for an agent; this one answers the agent.
    let mut remote: Vec<String> = vec!["task".into(), action.into(), uuid.into(), "--project".into(), project.0.clone(), "--json".into()];
    if action == "send" && option(args, "--session")?.is_some() {
        remote.extend(["--session".into(), session.session.clone()]);
    }
    if let Some(reason) = option(args, "--reason")?.filter(|_| action == "cancel") {
        remote.extend(["--reason".into(), reason.into()]);
    }
    if let Actor::Agent(agent) = who {
        remote.extend(["--agent".into(), format!("{}/{}", agent.machine, agent.session)]);
    }
    let reply = crate::swarm::remote_hgs(config, alias, &remote).map_err(|e| {
        fail(format!("{e:#}").replacen(&format!("{alias}: hgs: "), &format!("{alias}: "), 1))
    })?;
    // Its copy comes back now (its identity was checked a moment ago); when
    // that fails the next sync brings it, and `synced` says the copy here is behind.
    let synced = match crate::swarm::exchange_tasks(config, alias, &project.0) {
        Ok(()) => true,
        Err(error) => {
            eprintln!("hgs: {error:#}");
            false
        }
    };
    let task = store(config, &project.0)?.find(uuid).map_err(fail)?;
    let mut out = json!({"task": view(&task, &project.1, true, machines)?, "synced": synced});
    for key in ["receipt", "cancel_message"] {
        if let Some(value) = reply.get(key) {
            out[key] = value.clone();
        }
    }
    Ok(out)
}

/// Cancels here a task whose session's computer cannot be reached: it moves to
/// In review, its session is not told, and that computer's copy follows with
/// the next sync.
fn cancel_here(
    config: &Config,
    machines: &Machines,
    project: &(String, String),
    uuid: &str,
    args: &[String],
    who: &Actor,
    reason: String,
) -> Result<Value> {
    let store = store(config, &project.0)?;
    let mut task = store.find(uuid).map_err(fail)?;
    let extra = cancel(&store, &mut task, &project.1, args, who, Some(reason))?;
    store.save(&mut task).map_err(fail)?;
    let mut out = json!({"task": view(&task, &project.1, true, machines)?});
    out["cancel_message"] = extra["cancel_message"].clone();
    Ok(out)
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

/// Moves the task to In review and pastes the cancel message; the turn is not
/// stopped. `undelivered` says why the message cannot reach the session.
fn cancel(
    store: &ProjectStore,
    task: &mut TaskDoc,
    project_name: &str,
    args: &[String],
    who: &Actor,
    undelivered: Option<String>,
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
    let pasted = match undelivered {
        Some(reason) => Err(reason),
        None => crate::state::submit_task_text(&session.session, &message).map_err(|e| e.to_string()),
    };
    Ok(match pasted {
        Ok(receipt) => json!({"cancel_message": {"sent": true, "receipt": receipt}}),
        Err(error) => {
            task.note(&format!("Cancel message not sent: {error}"), who, now())
                .map_err(fail)?;
            json!({"cancel_message": {"sent": false, "error": error}})
        }
    })
}

fn settings(config: &Config, args: &[String]) -> Result<Value> {
    let invalid = |e: &dyn std::fmt::Display| fail(format!("invalid settings: {e}"));
    let changes = if flag(args, "--set") {
        match serde_json::from_str(&stdin_text("settings", "")?).map_err(|e| invalid(&e))? {
            Value::Object(changes) => Some(changes),
            _ => return Err(invalid(&"expected a JSON object")),
        }
    } else {
        None
    };
    let who = actor(&machines(config)?.here, args, false)?;
    let (id, name) = project_arg(config, args, &who)?;
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
    Ok(json!({"project": {"id": id, "name": name}, "settings": doc.settings()}))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn claude_and_codex_read_the_instructions_others_do_not() {
        for agent in ["claude", "codex"] {
            let context = session_context(agent).unwrap();
            assert_eq!(context["hookSpecificOutput"]["hookEventName"], "SessionStart");
            let text = context["hookSpecificOutput"]["additionalContext"].as_str().unwrap();
            for phrase in [
                "Zerus tasks are this project's shared task list.",
                "\"поставь таск\"",
                "hgs task report <ref>",
                "hgs guide tasks",
            ] {
                assert!(text.contains(phrase), "{phrase}");
            }
        }
        assert!(session_context("kimi").is_none());
    }
}
