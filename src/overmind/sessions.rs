//! Calls from the session host, so that tasks follow its sessions.
use super::model::{Actor, Draft};
use super::store::Store;
use anyhow::Result;
use serde_json::json;
use std::path::Path;

/// Records that a session on this machine was renamed, so pins, assignees and
/// reviewers follow it. A rename never fails because of Overmind, and a
/// machine that never used Overmind gets no store.
pub fn session_renamed(old: &str, new: &str) {
    if old == new {
        return;
    }
    let recorded = crate::config::Config::load()
        .map_err(|e| anyhow::anyhow!(e.message))
        .and_then(|config| {
            let dir = config.state.join("overmind");
            if !dir.join("overmind.sqlite3").exists() {
                return Ok(());
            }
            record_rename(&dir, &crate::swarm::local_node_id(&config)?, old, new)
        });
    if let Err(error) = recorded {
        eprintln!("hgs: Overmind tasks still name {old}: {error:#}");
    }
}

fn record_rename(dir: &Path, machine: &str, old: &str, new: &str) -> Result<()> {
    let mut store = Store::open(dir, machine)?;
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
}
