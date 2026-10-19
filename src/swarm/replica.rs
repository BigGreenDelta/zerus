//! Task replicas between members. After the catalog, every project whose
//! digest differs exchanges inventories and the updates each side lacks, in
//! pages; members whose hgs has no replica support are skipped and named in
//! Settings (docs/superpowers/specs/2026-10-18-task-replication-loro-design.md).
use super::{store::LockedStore, transport};
use crate::config::Config;
use anyhow::{bail, ensure, Result};
use base64::{engine::general_purpose::STANDARD, Engine};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::path::PathBuf;
use zerus_replica::{Document, Inventory, ProjectStore, Update, SCHEMA};

/// The replica exchange this hgs speaks; members with another one are skipped.
pub const PROTOCOL: u64 = 1;
/// The most update bytes one page carries; a larger document travels alone.
const PAGE: usize = 1 << 20;
/// Pages of one project in one sync; the next sync continues.
const MAX_PAGES: usize = 64;

fn root(config: &Config) -> PathBuf {
    config.state.join("replica")
}

/// Each stored project's inventory digest.
fn digests(config: &Config) -> Result<BTreeMap<String, String>> {
    let root = root(config);
    let mut out = BTreeMap::new();
    for id in ProjectStore::stored(&root)? {
        let store = ProjectStore::open(&root, &id)?;
        out.insert(id, store.inventory()?.0.digest());
    }
    Ok(out)
}

/// The replica capability `hello` advertises: the protocol, the highest task
/// schema this hgs reads and every stored project's digest.
pub fn hello(config: &Config) -> Result<Value> {
    Ok(json!({"protocol": PROTOCOL, "schema": SCHEMA, "projects": digests(config)?}))
}

/// Changes whenever any stored project changes; the worker exchanges at once.
pub fn fingerprint(config: &Config) -> Result<String> {
    Ok(format!("{:x}", Sha256::digest(serde_json::to_vec(&digests(config)?)?)))
}

/// One update on the wire: the document and its Loro updates in base64.
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Wire {
    document: Document,
    bytes: String,
}

fn encode(updates: Vec<Update>) -> Vec<Wire> {
    updates
        .into_iter()
        .map(|u| Wire {
            document: u.document,
            bytes: STANDARD.encode(u.bytes),
        })
        .collect()
}

fn decode(wire: Vec<Wire>) -> Result<Vec<Update>> {
    wire.into_iter()
        .map(|w| {
            Ok(Update {
                document: w.document,
                bytes: STANDARD.decode(w.bytes)?,
            })
        })
        .collect()
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Request {
    project: String,
    #[serde(default)]
    inventory: Option<Inventory>,
    #[serde(default)]
    updates: Vec<Wire>,
}

/// `swarm replica-inventory`: what this member holds of the project.
pub fn inventory(config: &Config, request: Request) -> Result<Value> {
    let store = ProjectStore::open(&root(config), &request.project)?;
    Ok(json!({"inventory": store.inventory()?.0}))
}

/// `swarm replica-exchange`: imports the requester's updates and answers with
/// a page of what its inventory lacks.
pub fn exchange(config: &Config, request: Request) -> Result<Value> {
    let theirs = request
        .inventory
        .ok_or_else(|| anyhow::anyhow!("a replica exchange needs the requester's inventory"))?;
    let store = ProjectStore::open(&root(config), &request.project)?;
    let imported = store.import_updates(decode(request.updates)?)?;
    let (updates, more) = store.updates_for(&theirs, PAGE)?;
    Ok(json!({"imported": imported.changed, "refused": imported.refused, "updates": encode(updates), "more": more}))
}

/// Exchanges every project whose digest differs with the member behind
/// `alias`, whose `hello` answered `identity`.
pub fn sync_peer(config: &Config, alias: &str, identity: &Value) -> Result<()> {
    let remote = &identity["replica"];
    let supported = remote["protocol"] == PROTOCOL;
    let mut store = LockedStore::load(config)?;
    if supported {
        store.replica_outdated.remove(alias);
    } else {
        let node = identity["node_id"].as_str().unwrap_or_default().to_owned();
        store.replica_outdated.insert(alias.into(), node);
    }
    store.save()?;
    drop(store);
    if !supported {
        return Ok(());
    }
    let local = digests(config)?;
    let theirs: BTreeMap<String, String> =
        serde_json::from_value(remote["projects"].clone()).unwrap_or_default();
    let projects: BTreeSet<&String> = local.keys().chain(theirs.keys()).collect();
    for project in projects {
        if local.get(project) != theirs.get(project) {
            exchange_project(config, alias, project)?;
        }
    }
    Ok(())
}

fn exchange_project(config: &Config, alias: &str, project: &str) -> Result<()> {
    let root = root(config);
    for _ in 0..MAX_PAGES {
        let reply = transport::remote(config, alias, "replica-inventory", Some(&json!({"project": project})))?;
        let theirs: Inventory = serde_json::from_value(reply["inventory"].clone())?;
        // The project's lock is held only between network calls.
        let (mine, page, more_mine) = {
            let store = ProjectStore::open(&root, project)?;
            let (mine, _) = store.inventory()?;
            let (page, more) = store.updates_for(&theirs, PAGE)?;
            (mine, page, more)
        };
        let response = transport::remote(
            config,
            alias,
            "replica-exchange",
            Some(&json!({"project": project, "inventory": mine, "updates": encode(page)})),
        )?;
        let updates = decode(serde_json::from_value(response["updates"].clone())?)?;
        ensure!(response["more"].is_boolean(), "{alias} sent an invalid replica exchange");
        if !updates.is_empty() {
            let imported = ProjectStore::open(&root, project)?.import_updates(updates)?;
            // A refused document is named in the worker's log; the others are kept.
            for refused in imported.refused {
                eprintln!("hgs swarm: {alias}: task {} of {project} was refused: {}", refused.uuid, refused.error);
            }
        }
        if !more_mine && response["more"] != true {
            return Ok(());
        }
    }
    bail!("the tasks of {project} did not finish in {MAX_PAGES} pages; the next sync continues")
}
