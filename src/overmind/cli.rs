//! `hgs overmind`: JSON in, JSON out. Refusals are printed as JSON on stdout
//! so that the desktop, the phone and MCP can show their fields.
use super::commands::{self, Ctx};
use super::model::{Actor, ActorKind, Refusal};
use super::store::Store;
use crate::{cli, config::Config};
use anyhow::{bail, ensure, Result};
use serde_json::{json, Value};
use std::io::Read;

pub const MAX_REQUEST: u64 = 29 * 1024 * 1024;

const HUMAN_USAGE: &str = "usage: hgs [@host] overmind <command> [--json]   (JSON request on stdin)
  board --json {project, since?}           tasks of a project, optionally changed since a cursor
  show <ref> | show --json {task}          one task with its prompt and journal
  create --json {project, prompt, title?, stage?, priority?, labels?, pinned?, attachments?, parent?, follows?}
  edit --json {task, title?, prompt?, priority?, attachments?}
  move --json {task, to}  |  done --json {task}  |  reopen --json {task}
  reorder --json {task, before?}  |  pin --json {task, session?}
  label --json {task, label, present}  |  link --json {task, relation, target, present}
  comment --json {task, text}  |  delete --json {task}
  workflow get --json {project}  |  workflow set --json {project, workflow, moves?}
  rev                                      change cursor for watchers
Write requests may carry a request_id (UUID); repeating it replays the result.
";

const AGENT_USAGE: &str = "usage: hgs overmind <command> --json   (JSON request on stdin)
  mine                                     tasks assigned to this session or sent to it for review
  show <ref>                               the task, its prompt and journal
  comment --json {task, text}
  advance --json {task, to, spec}          Design -> Build: your plan
  advance --json {task, to, worklog, evidence, root_cause?}   Build -> Review: what you did and its commit
  call-human --json {task, question}       park the task until a person decides
  return --json {task, reason}             give back a task blocked by something external
  decompose --json {task, subtasks: [{title?, prompt}], ordered?}
  file --json {prompt, breaks, title?, related?, project?}   record an out-of-scope finding in Backlog
  attach --json {task, name, mime, data_base64, note?}
  review --json {task, verdict: approve|needs_work, report}
You can never move a task to Done: a person signs it off.
";

fn usage(actor: &Actor) -> &'static str {
    if actor.kind == ActorKind::Agent {
        AGENT_USAGE
    } else {
        HUMAN_USAGE
    }
}

pub fn dispatch(config: &Config, args: &[String], dry: bool) -> cli::Result<i32> {
    if dry {
        return Err(cli::Error::new(1, "overmind commands do not support --dry-run"));
    }
    match run(config, args) {
        Ok(Some(value)) => {
            println!("{value}");
            Ok(0)
        }
        Ok(None) => Ok(0),
        Err(error) => {
            if let Some(refusal) = error.downcast_ref::<Refusal>() {
                println!("{}", json!({"ok": false, "error": refusal}));
                return Err(cli::Error::new(1, refusal.message.clone()));
            }
            Err(cli::Error::new(1, format!("{error:#}")))
        }
    }
}

fn read_stdin() -> Result<Value> {
    let mut bytes = Vec::new();
    std::io::stdin().take(MAX_REQUEST + 1).read_to_end(&mut bytes)?;
    ensure!(bytes.len() as u64 <= MAX_REQUEST, "the overmind request exceeds 29 MiB");
    Ok(serde_json::from_slice(&bytes)?)
}

fn run(config: &Config, args: &[String]) -> Result<Option<Value>> {
    let machine = crate::swarm::local_node_id(config)?;
    let actor = Actor::human(&machine);
    let cwd = None;
    let command = args.first().map(String::as_str).unwrap_or("--help");
    if matches!(command, "-h" | "--help" | "help") {
        print!("{}", usage(&actor));
        return Ok(None);
    }
    let rest: Vec<&str> = args[1..].iter().map(String::as_str).collect();
    let json_flag = rest.last() == Some(&"--json");
    let positional: Vec<&str> = rest.iter().copied().filter(|a| *a != "--json").collect();
    let request = if json_flag { read_stdin()? } else { json!({}) };
    let mut store = Store::open(&config.state.join("overmind"), &machine)?;
    let mut ctx = Ctx { store: &mut store, actor, cwd };
    let value = match (command, positional.as_slice()) {
        ("rev", []) => commands::rev(&mut ctx)?,
        ("board", []) => commands::board(&mut ctx, request)?,
        ("show", [reference]) => commands::show(&mut ctx, json!({"task": reference}))?,
        ("show", []) => commands::show(&mut ctx, request)?,
        ("create", []) => commands::create(&mut ctx, request)?,
        ("edit", []) => commands::edit(&mut ctx, request)?,
        ("move", []) => commands::move_task(&mut ctx, request)?,
        ("done", []) => commands::done(&mut ctx, request)?,
        ("reopen", []) => commands::reopen(&mut ctx, request)?,
        ("reorder", []) => commands::reorder(&mut ctx, request)?,
        ("pin", []) => commands::pin(&mut ctx, request)?,
        ("label", []) => commands::label(&mut ctx, request)?,
        ("link", []) => commands::link(&mut ctx, request)?,
        ("comment", []) => commands::comment(&mut ctx, request)?,
        ("delete", []) => commands::delete(&mut ctx, request)?,
        ("workflow", ["get"]) => commands::workflow_get(&mut ctx, request)?,
        ("workflow", ["set"]) => commands::workflow_set(&mut ctx, request)?,
        _ => bail!("unknown overmind command; see hgs overmind --help"),
    };
    Ok(Some(value))
}
