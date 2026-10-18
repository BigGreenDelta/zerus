//! `hgs task`: the project task tracker on this computer. Tasks are Loro
//! documents kept by `zerus-replica` below `<state dir>/replica`; every
//! command prints JSON. See docs/superpowers/specs/2026-10-13-project-task-queue-design.md
//! and docs/superpowers/specs/2026-10-18-task-replication-loro-design.md.
//!
//! Lock order: catalog lookups (project, machine identity) happen before the
//! project store is opened, and stdin is read before it too.
use crate::cli::{Error, Result};
use crate::config::Config;
use serde_json::{json, Value};
use std::io::Read;
use std::time::{SystemTime, UNIX_EPOCH};
use zerus_replica::{
    task_prompt, Actor, NewTask, Position, ProjectStore, SessionKey, Settings, Stage, Target,
    TaskDoc, Tools, MAX_TEXT,
};

const USAGE: &str = "usage: hgs task list --project P [--stage S] | get REF | add --project P [--backlog] [--target SESSION|any] [--position N] | edit REF | move REF --top|--before REF|--after REF | stage REF backlog|todo|done | target REF SESSION|any | send REF [--session SESSION] | cancel REF [--reason TEXT] | report REF | comment REF | settings --project P [--set]
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

fn machine(config: &Config) -> Result<String> {
    crate::swarm::local_node_id(config).map_err(fail)
}

/// The agent of the session that runs this command, as hooks know it (both
/// `HGS_SESSION` and `HGS_RUN_ID`), or the person.
fn actor(here: &str) -> Actor {
    let var = |name| std::env::var(name).ok().filter(|v| !v.is_empty());
    match (var("HGS_SESSION"), var("HGS_RUN_ID")) {
        (Some(session), Some(_)) => Actor::Agent(SessionKey {
            machine: here.to_owned(),
            session,
        }),
        _ => Actor::Person,
    }
}

fn view(task: &TaskDoc, project_name: &str, with_log: bool) -> Result<Value> {
    let mut value = serde_json::to_value(task.view().map_err(fail)?).map_err(fail)?;
    value["ref"] = json!(format!("{project_name}/{}", task.id()));
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
            let task = store(config, &r.project.0)?
                .find(&r.task)
                .map_err(fail)?;
            print(json!({"project": {"id": r.project.0, "name": r.project.1}, "task": view(&task, &r.project.1, true)?}))
        }
        "add" => add(config, args),
        "edit" | "move" | "stage" | "target" | "comment" | "report" => {
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
    let store = store(config, &id)?;
    let (_, damaged) = store.tasks().map_err(fail)?;
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
        for task in store.ordered(stage).map_err(fail)? {
            out.push(view(&task, &name, false)?);
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
    let here = machine(config)?;
    let author = actor(&here);
    let stage = if flag(args, "--backlog") {
        Stage::Backlog
    } else {
        Stage::Todo
    };
    // A task an agent files is always pinned to that agent's session.
    let target = match (&author, option(args, "--target")?) {
        (Actor::Agent(_), Some(_)) => {
            return Err(fail(
                "a task an agent files is pinned to its own session; --target is for people",
            ))
        }
        (Actor::Agent(own), None) => Target::Session(own.clone()),
        (Actor::Person, None | Some("any")) => Target::Any,
        (Actor::Person, Some(session)) => Target::Session(SessionKey {
            machine: here.clone(),
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
    print(json!({"task": view(&task, &name, true)?}))
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
    let here = machine(config)?;
    let who = actor(&here);
    let store = store(config, project_id)?;
    let mut task = store.find(&r.task).map_err(fail)?;
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
            let target = if value == "any" {
                Target::Any
            } else {
                Target::Session(SessionKey {
                    machine: here.clone(),
                    session: value.clone(),
                })
            };
            task.set_target(&target, &who, now()).map_err(fail)?;
        }
        _ => return Err(fail(USAGE)),
    }
    store.save(&mut task).map_err(fail)?;
    let mut out = json!({"task": view(&task, project_name, true)?});
    for (key, value) in extra.as_object().expect("object") {
        out[key] = value.clone();
    }
    print(out)
}

fn settings(config: &Config, args: &[String]) -> Result<i32> {
    let wanted = if flag(args, "--set") {
        Some(
            serde_json::from_str::<Settings>(&stdin_text("settings")?)
                .map_err(|e| fail(format!("invalid settings: {e}")))?,
        )
    } else {
        None
    };
    let (id, name) = project(
        config,
        option(args, "--project")?.ok_or_else(|| fail(USAGE))?,
    )?;
    let store = store(config, &id)?;
    let mut doc = store.project().map_err(fail)?;
    if let Some(wanted) = wanted {
        doc.set_settings(&wanted).map_err(fail)?;
        store.save_project(&doc).map_err(fail)?;
    }
    print(json!({"project": {"id": id, "name": name}, "settings": doc.settings()}))
}
