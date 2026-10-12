//! Calls from the session host, so that tasks follow its sessions. A hook never
//! fails the host's command or waits long because of Overmind, and a machine
//! that never used Overmind gets no store.
use super::commands::workflow;
use super::deliver;
use super::fold::{fold_many, Task};
use super::model::{Actor, Draft, SessionRef};
use super::store::{Filter, Store, Tx};
use super::workflow::Role;
use anyhow::Result;
use serde_json::json;
use std::time::Duration;

/// Set on tasks whose assignee session ended; a change of assignee clears it.
const ORPHANED: &str = "orphaned";

/// Records that a session on this machine was renamed, so pins, assignees and
/// reviewers follow it.
pub fn session_renamed(old: &str, new: &str) {
    if old == new {
        return;
    }
    let recorded = with_store(|store, machine| {
        store.write(|tx| {
            tx.append(&Draft {
                actor: Actor::human(machine),
                task: String::new(),
                project: String::new(),
                kind: "session_renamed".into(),
                base: String::new(),
                request_id: String::new(),
                payload: json!({"machine": machine, "old": old, "new": new}),
            })
        })?;
        Ok(())
    });
    if let Err(error) = recorded {
        eprintln!("hgs: Overmind tasks still name {old}: {error:#}");
    }
}

/// Records that a session on this machine stopped taking input for good:
/// archived, terminated, forgotten or killed. The tasks it holds in active or
/// needs-human stages get the `orphaned` flag, written as a person on this
/// machine, so that someone returns them to the queue.
pub fn session_ended(name: &str) {
    let flagged = with_store(|store, machine| {
        let session = SessionRef { machine: machine.into(), name: name.into() };
        // Most sessions hold no task: look before taking the write lock.
        if store.read(|tx| held(tx, &session))?.is_empty() {
            return Ok(());
        }
        let actor = Actor::human(machine);
        store.write(|tx| {
            for task in held(tx, &session)? {
                deliver::flag(tx, &actor, &task, ORPHANED, true)?;
            }
            Ok(())
        })
    });
    if let Err(error) = flagged {
        eprintln!("hgs: Overmind tasks of {name} were not flagged {ORPHANED}: {error:#}");
    }
}

/// Runs `record` with this machine's store, waiting at most a second for its
/// other writers. The store is opened only after the machine identity was saved.
fn with_store(record: impl FnOnce(&mut Store, &str) -> Result<()>) -> Result<()> {
    let config = crate::config::Config::load().map_err(|e| anyhow::anyhow!(e.message))?;
    let dir = config.state.join("overmind");
    match crate::swarm::saved_node_id(&config) {
        Some(machine) if dir.join("overmind.sqlite3").exists() => {
            let mut store = Store::open(&dir, &machine)?;
            store.wait_for_writers(Duration::from_secs(1))?;
            record(&mut store, &machine)
        }
        _ => Ok(()),
    }
}

/// Tasks the session holds in active or needs-human stages that are not flagged
/// yet and that this version may change.
fn held(tx: &Tx, session: &SessionRef) -> Result<Vec<Task>> {
    let mut tasks = Vec::new();
    for task in fold_many(&tx.load(&Filter::All)?).into_values() {
        if task.deleted || task.read_only || task.flags.contains(ORPHANED) || task.assignee.as_ref() != Some(session) {
            continue;
        }
        if matches!(workflow(tx, &task.project)?.role(&task.stage), Some(Role::Active | Role::NeedsHuman)) {
            tasks.push(task);
        }
    }
    Ok(tasks)
}
