//! What a phone exchanges with this computer through the relay: `hgs task
//! digests --json` for the heartbeat and `hgs task exchange --json` for one
//! page (docs/superpowers/specs/2026-10-18-task-replication-loro-design.md, "Phone").
use super::{fail, Result};
use crate::config::Config;
use serde_json::{json, Value};
use std::io::Read;
use std::path::PathBuf;
use std::time::Duration;
use zerus_replica::phone::{answer, Request};
use zerus_replica::{Inventory, ProjectStore};

/// The most a request may be: the relay's canonical 1 MiB.
const MAX_REQUEST: u64 = 1 << 20;
/// How long an exchange waits for a project another command holds.
const PROJECT_WAIT: Duration = Duration::from_secs(2);
/// How long the digests wait for each busy project: one held project is left
/// out this time, and the others keep theirs within the connector's budget.
const DIGEST_WAIT: Duration = Duration::from_millis(250);

fn root(config: &Config) -> PathBuf {
    config.state.join("replica")
}

/// The projects this computer answers for: alive in the catalog or stored here.
fn known(config: &Config) -> Result<(Vec<String>, Vec<String>)> {
    let alive = crate::swarm::projects(config)
        .map_err(fail)?
        .into_iter()
        .map(|(id, _)| id)
        .collect();
    let stored = ProjectStore::stored(&root(config)).map_err(fail)?;
    Ok((alive, stored))
}

/// `hgs task digests --json`: each project's phone-view digest. A project
/// not stored here has the empty view; one that cannot be read now is left
/// out and named on stderr.
pub(super) fn digests(config: &Config) -> Result<Value> {
    let (alive, stored) = known(config)?;
    let mut projects = serde_json::Map::new();
    for id in alive.iter().filter(|id| !stored.contains(id)) {
        projects.insert(id.clone(), json!({"digest": Inventory::default().digest()}));
    }
    for id in stored {
        match ProjectStore::open_within(&root(config), &id, DIGEST_WAIT) {
            Ok(Some(store)) => match store.live_inventory() {
                Ok((inventory, _)) => {
                    projects.insert(id, json!({"digest": inventory.digest()}));
                }
                Err(error) => eprintln!("hgs: the tasks of {id:?} cannot be read: {error}"),
            },
            Ok(None) => eprintln!("hgs: the tasks of {id:?} are busy; they are left out this time"),
            Err(error) => eprintln!("hgs: the tasks of {id:?} cannot be read: {error}"),
        }
    }
    Ok(json!({"schema": 1, "projects": projects}))
}

/// `hgs task exchange --json`: imports the phone's updates and answers with a
/// page of what it lacks (`zerus_replica::phone::answer`).
pub(super) fn exchange(config: &Config) -> Result<Value> {
    let mut bytes = Vec::new();
    std::io::stdin()
        .take(MAX_REQUEST + 1)
        .read_to_end(&mut bytes)
        .map_err(fail)?;
    if bytes.len() as u64 > MAX_REQUEST {
        return Err(fail("the exchange request exceeds 1 MiB"));
    }
    let request: Request = serde_json::from_slice(&bytes)
        .map_err(|e| fail(format!("invalid exchange request: {e}")))?;
    request.check().map_err(fail)?;
    let (alive, stored) = known(config)?;
    if !alive.contains(&request.project) && !stored.contains(&request.project) {
        return Err(fail(format!("no project {:?} here", request.project)));
    }
    let store = ProjectStore::open_within(&root(config), &request.project, PROJECT_WAIT)
        .map_err(fail)?
        .ok_or_else(|| fail("the project's tasks are busy; try again"))?;
    serde_json::to_value(answer(&store, request).map_err(fail)?).map_err(fail)
}
