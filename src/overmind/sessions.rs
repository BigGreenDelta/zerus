//! Calls from the session host, so that tasks follow its sessions. A hook never
//! fails the host's command or waits long because of Overmind, and a machine
//! that never used Overmind gets no store.
use super::commands::simple;
use super::fold::{fold_many, project_workflow, Task};
use super::model::{Actor, Draft, Event, SessionRef, EVENT_VERSION};
use super::store::{Filter, Store, Tx};
use super::workflow::{Role, Workflow};
use anyhow::Result;
use serde_json::{json, Value};
use std::collections::BTreeMap;
use std::time::Duration;

/// Set on tasks whose assignee session ended; a change of assignee or an
/// accepted action of the assignee session clears it.
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
/// machine, so that someone returns them to the queue. The flag names the
/// session, so it lands only on tasks that the session still holds when it sorts.
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
                simple(tx, &actor, &task, "flag_set", json!({"flag": ORPHANED, "value": true, "session": session}), "")?;
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
    Ok(holding(tx, session)?.into_iter().filter(|task| !task.read_only && !task.flags.contains(ORPHANED)).collect())
}

/// Tasks the session holds in active or needs-human stages, in every project.
/// The events are folded once and each project's workflow is read once.
pub fn holding(tx: &Tx, session: &SessionRef) -> Result<Vec<Task>> {
    let events = tx.load(&Filter::All)?;
    let all: Vec<&Event> = events.iter().collect();
    let mut workflows: BTreeMap<String, Workflow> = BTreeMap::new();
    let mut tasks = Vec::new();
    for task in fold_many(&events).into_values() {
        if task.deleted || task.assignee.as_ref() != Some(session) {
            continue;
        }
        let workflow = workflows.entry(task.project.clone()).or_insert_with(|| project_workflow(&all, &task.project));
        if matches!(workflow.role(&task.stage), Some(Role::Active | Role::NeedsHuman)) {
            tasks.push(task);
        }
    }
    Ok(tasks)
}

/// The name a session of this machine has now. A pin written on another machine
/// can name a session that this machine renamed concurrently: the rename sorts
/// before the pin, so the fold keeps the name as written. The host follows its
/// own renames, oldest first, past every name that no session here uses.
pub fn current_pin(tx: &Tx, pinned: &SessionRef, machine: &str) -> Result<SessionRef> {
    if pinned.machine != machine {
        return Ok(pinned.clone());
    }
    let shared = tx.load(&Filter::Shared(String::new()))?;
    let name = current_name(&shared, machine, &pinned.name, &crate::state::session_tracked);
    Ok(SessionRef { machine: pinned.machine.clone(), name })
}

/// `name` after the renames of `machine` in `events`, oldest first, stopping
/// at a name that a tracked session uses.
pub fn current_name(events: &[Event], machine: &str, name: &str, tracked: &dyn Fn(&str) -> bool) -> String {
    let text = |event: &Event, key: &str| event.payload.get(key).and_then(Value::as_str).unwrap_or_default().to_owned();
    let mut renames: Vec<&Event> = events
        .iter()
        .filter(|e| e.task.is_empty() && e.kind == "session_renamed" && e.v <= EVENT_VERSION)
        // Only the host renames its sessions, as in the fold.
        .filter(|e| e.actor.machine == machine && text(e, "machine") == machine)
        .collect();
    renames.sort_by(|a, b| a.order().cmp(&b.order()));
    let mut current = name.to_owned();
    for rename in renames {
        if text(rename, "old") == current {
            if tracked(&current) {
                break;
            }
            current = text(rename, "new");
        }
    }
    current
}

#[cfg(test)]
mod tests {
    use super::*;

    const HERE: &str = "11111111-1111-4111-8111-111111111111";
    const THERE: &str = "22222222-2222-4222-8222-222222222222";

    fn renamed(lamport: u64, author: &str, machine: &str, old: &str, new: &str) -> Event {
        Event {
            id: format!("rename-{lamport}"), origin: author.into(), seq: lamport, lamport, at: lamport, v: EVENT_VERSION,
            actor: Actor::human(author), task: String::new(), project: String::new(), kind: "session_renamed".into(),
            base: String::new(), request_id: String::new(), payload: json!({"machine": machine, "old": old, "new": new}),
        }
    }

    #[test]
    fn a_name_follows_this_machines_renames_past_names_no_session_uses() {
        // Out of order on purpose: renames apply oldest first.
        let events = vec![
            renamed(5, HERE, HERE, "mid", "new"),
            renamed(2, HERE, HERE, "old", "mid"),
            renamed(3, THERE, HERE, "new", "forged"),
            renamed(4, THERE, THERE, "old", "theirs"),
        ];
        let nothing = |_: &str| false;
        assert_eq!(current_name(&events, HERE, "old", &nothing), "new");
        assert_eq!(current_name(&events, HERE, "mid", &nothing), "new");
        assert_eq!(current_name(&events, HERE, "new", &nothing), "new", "only the host renames its sessions");
        assert_eq!(current_name(&events, HERE, "other", &nothing), "other");
        // A session that uses a name again stops the chain there.
        assert_eq!(current_name(&events, HERE, "old", &|name: &str| name == "old"), "old");
        assert_eq!(current_name(&events, HERE, "old", &|name: &str| name == "mid"), "mid");
        let mut newer = renamed(1, HERE, HERE, "old", "elsewhere");
        newer.v = EVENT_VERSION + 1;
        assert_eq!(current_name(&[newer], HERE, "old", &nothing), "old");
    }
}
