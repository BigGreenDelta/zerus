//! What a phone exchanges with this computer through the relay: `hgs task
//! digests --json` for the heartbeat and `hgs task exchange --json` for one
//! page (docs/superpowers/specs/2026-10-18-task-replication-loro-design.md, "Phone").
use super::{fail, Result};
use crate::config::Config;
use base64::{engine::general_purpose::STANDARD, Engine};
use serde::Deserialize;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::io::Read;
use std::path::PathBuf;
use std::time::Duration;
use zerus_replica::{Document, Inventory, ProjectStore, Update};

/// The update bytes one answer carries; base64 and JSON keep it under the relay's 1 MiB.
const PAGE: usize = 512 * 1024;
/// The most a request may be: the relay's canonical 1 MiB.
const MAX_REQUEST: u64 = 1 << 20;
/// How long a command waits for a project another command holds.
const PROJECT_WAIT: Duration = Duration::from_secs(2);

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
        match ProjectStore::open_within(&root(config), &id, PROJECT_WAIT) {
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

#[derive(Deserialize)]
struct Request {
    schema: u32,
    project: String,
    /// Everything the phone holds of the project.
    inventory: Inventory,
    #[serde(default)]
    updates: Vec<WireUpdate>,
    /// The next part of an update too large for one answer.
    #[serde(default, rename = "continue")]
    resume: Option<Continue>,
}

#[derive(Deserialize)]
struct WireUpdate {
    task: Option<String>,
    bytes: String,
}

#[derive(Deserialize)]
struct Continue {
    task: Option<String>,
    sha256: String,
    offset: usize,
}

fn document(task: Option<String>) -> Document {
    task.map_or(Document::Project, Document::Task)
}

fn wire(document: &Document) -> Value {
    match document {
        Document::Project => Value::Null,
        Document::Task(uuid) => json!(uuid),
    }
}

/// `ours` narrowed to one document.
fn only(ours: &Inventory, document: &Document) -> Inventory {
    let mut one = Inventory::default();
    match document {
        Document::Project => one.project = ours.project.clone(),
        Document::Task(uuid) => {
            if let Some(version) = ours.tasks.get(uuid) {
                one.tasks.insert(uuid.clone(), version.clone());
            }
        }
    }
    one
}

/// The part of `update` from `offset`, at most one page, as the phone reads it.
fn part(update: &Update, offset: usize) -> Value {
    let end = (offset + PAGE).min(update.bytes.len());
    json!({
        "task": wire(&update.document),
        "size": update.bytes.len(),
        "sha256": format!("{:x}", Sha256::digest(&update.bytes)),
        "offset": offset,
        "bytes": STANDARD.encode(&update.bytes[offset..end]),
    })
}

/// `hgs task exchange --json`: imports the phone's updates and answers with a
/// page of what it lacks of the documents it holds and of the live tasks; an
/// update larger than a page goes in parts. An answer with a part always has
/// `more`: the phone asks again once it holds the whole update. An answer with
/// nothing for the phone names what this computer holds of the phone's view,
/// so the phone can send what it lacks. Merges are idempotent, so a repeated
/// request is safe.
pub(super) fn exchange(config: &Config) -> Result<Value> {
    let mut bytes = Vec::new();
    std::io::stdin()
        .take(MAX_REQUEST + 1)
        .read_to_end(&mut bytes)
        .map_err(fail)?;
    if bytes.len() as u64 > MAX_REQUEST {
        return Err(fail("the exchange request exceeds 1 MiB"));
    }
    let request: Request =
        serde_json::from_slice(&bytes).map_err(|e| fail(format!("invalid exchange request: {e}")))?;
    if request.schema != 1 {
        return Err(fail(format!("unknown exchange schema {}", request.schema)));
    }
    let (alive, stored) = known(config)?;
    if !alive.contains(&request.project) && !stored.contains(&request.project) {
        return Err(fail(format!("no project {:?} here", request.project)));
    }
    let updates = request
        .updates
        .into_iter()
        .map(|update| {
            Ok(Update {
                document: document(update.task),
                bytes: STANDARD
                    .decode(update.bytes)
                    .map_err(|_| fail("invalid base64 in an update"))?,
            })
        })
        .collect::<Result<Vec<_>>>()?;
    let store = ProjectStore::open_within(&root(config), &request.project, PROJECT_WAIT)
        .map_err(fail)?
        .ok_or_else(|| fail("the project's tasks are busy; try again"))?;
    let imported = store.import_updates(updates).map_err(fail)?;
    let theirs = request.inventory;
    let ours = store.phone_inventory(&theirs).map_err(fail)?;
    let mut answer = json!({"schema": 1, "imported": imported.changed, "refused": imported.refused,
        "updates": [], "more": false});
    match request.resume {
        Some(resume) => {
            // The whole update again: its parts continue while it is unchanged.
            let wanted = document(resume.task);
            let (whole, _) = store
                .updates_for(&only(&ours, &wanted), &theirs, usize::MAX)
                .map_err(fail)?;
            if let Some(update) = whole.into_iter().find(|u| u.document == wanted) {
                let same = format!("{:x}", Sha256::digest(&update.bytes)) == resume.sha256
                    && resume.offset < update.bytes.len();
                answer["part"] = part(&update, if same { resume.offset } else { 0 });
                answer["more"] = json!(true);
            }
        }
        None => {
            let (page, more) = store.updates_for(&ours, &theirs, PAGE).map_err(fail)?;
            match page.as_slice() {
                [update] if update.bytes.len() > PAGE => {
                    answer["part"] = part(update, 0);
                    answer["more"] = json!(true);
                }
                _ => {
                    answer["updates"] = page
                        .iter()
                        .map(|u| json!({"task": wire(&u.document), "bytes": STANDARD.encode(&u.bytes)}))
                        .collect();
                    answer["more"] = json!(more);
                }
            }
        }
    }
    if answer.get("part").is_none() && answer["updates"].as_array().is_some_and(Vec::is_empty) {
        answer["inventory"] = json!(ours);
    }
    answer["digest"] = json!(store.live_inventory().map_err(fail)?.0.digest());
    Ok(answer)
}
