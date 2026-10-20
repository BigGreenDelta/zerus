//! Shared metadata only. Transport credentials and per-device presentation stay
//! local; the swarm-wide phone delivery choice is shared. All commands use one
//! locked, atomically replaced store.
mod catalog;
mod launch_project;
mod session_project;
#[cfg(unix)]
pub(crate) use session_project::validate_payload as validate_session_project;
mod model;
mod service;
mod store;
mod transport;

use crate::{cli, config::Config};
use anyhow::{bail, ensure, Result};
use serde_json::{json, Value};
use std::io::Read;
use store::LockedStore;

#[cfg(unix)]
pub(crate) use catalog::MOBILE_DELIVERY;
#[cfg(unix)]
pub(crate) use store::replace;

/// Machine UUIDs this computer itself obtained through each configured SSH
/// alias (`swarm hello`/`export`, then pinned by `bind`). Bindings stay local
/// and are never exchanged. Shared catalog data, including machine names,
/// redirects and the `machines` listing, comes from other members and is not
/// identity evidence. Lock-free like `preferences`; a damaged or foreign store
/// reports none.
#[cfg(unix)]
pub(crate) fn verified_bindings(
    config: &Config,
    local: &str,
) -> std::collections::BTreeMap<String, String> {
    #[derive(serde::Deserialize)]
    struct Local {
        node_id: String,
        #[serde(default)]
        bindings: std::collections::BTreeMap<String, store::Binding>,
    }
    std::fs::read(config.dir.join("swarm/catalog.json"))
        .ok()
        .and_then(|bytes| serde_json::from_slice::<Local>(&bytes).ok())
        .filter(|store| store.node_id == local)
        .map(|store| {
            store
                .bindings
                .into_iter()
                .filter(|(alias, binding)| {
                    binding.verified && !config.is_self(alias) && binding.id != local
                })
                .map(|(alias, binding)| (alias, binding.id))
                .collect()
        })
        .unwrap_or_default()
}

/// Shared preferences for `hgs ls --json --local`. Lock-free: the store is
/// replaced atomically and listings never wait for a sync. A missing or
/// unreadable catalog reports the defaults.
/// `{running, current}` for the background catalog worker.
pub(crate) fn worker_status(config: &Config) -> Value {
    service::status(config).json()
}

pub(crate) fn preferences(config: &Config) -> Value {
    std::fs::read(config.dir.join("swarm/catalog.json"))
        .ok()
        .and_then(|bytes| serde_json::from_slice::<store::Store>(&bytes).ok())
        .map_or_else(catalog::default_preferences, |store| {
            store.catalog.preferences()
        })
}

fn preference(config: &Config, args: &[String]) -> Result<Value> {
    let args: Vec<&str> = args.iter().map(String::as_str).collect();
    let args = args.strip_suffix(&["--json"]).unwrap_or(&args[..]);
    match args {
        ["get"] => Ok(LockedStore::load(config)?.data.catalog.preferences()),
        ["set", field, value] => {
            let mut store = LockedStore::load(config)?;
            let actor = store.data.node_id.clone();
            store
                .data
                .catalog
                .set(&actor, catalog::Key::preference(field), json!(value))?;
            store.save()?;
            let mut result = store.data.catalog.preferences();
            result["ok"] = json!(true);
            Ok(result)
        }
        _ => bail!("usage: hgs swarm preference get --json | set mobile_delivery VALUE --json"),
    }
}

/// Persist and return the machine identity without retaining the catalog lock.
pub(crate) fn local_node_id(config: &Config) -> Result<String> {
    let mut store = LockedStore::load(config)?;
    let id = uuid::Uuid::parse_str(&store.data.node_id)?;
    ensure!(
        !id.is_nil() && id.to_string() == store.data.node_id,
        "invalid persisted machine identity"
    );
    store.save()?;
    Ok(store.data.node_id.clone())
}

fn input() -> Result<Value> {
    let mut bytes = Vec::new();
    std::io::stdin()
        .take(transport::MAX_BYTES + 1)
        .read_to_end(&mut bytes)?;
    ensure!(
        bytes.len() as u64 <= transport::MAX_BYTES,
        "swarm request is too large"
    );
    Ok(serde_json::from_slice(&bytes)?)
}

pub fn dispatch(config: &Config, args: &[String], dry: bool) -> cli::Result<i32> {
    if dry {
        return Err(cli::Error::new(
            1,
            "swarm commands do not support --dry-run",
        ));
    }
    match execute(config, args) {
        Ok(value) => {
            println!("{}", value);
            Ok(0)
        }
        Err(error) => Err(cli::Error::new(1, format!("{error:#}"))),
    }
}

fn execute(config: &Config, args: &[String]) -> Result<Value> {
    let action = args.first().map(String::as_str).unwrap_or("get");
    let alias = || {
        args.get(1)
            .map(String::as_str)
            .ok_or_else(|| anyhow::anyhow!("choose a configured machine"))
    };
    // Commands that read or write preferences restart a worker too old to read them.
    let worker = matches!(action, "get" | "preference" | "exchange" | "join" | "sync")
        .then(|| service::supervise(config));
    match action {
        "session-project" | "assign-session" => {
            ensure!(args.len() == 2 && args[1] == "--json", "usage: hgs swarm {action} --json");
            Ok(session_project::execute(config, input()?, action == "assign-session"))
        }
        "assign-launch" => {
            ensure!(args.len() == 2 && args[1] == "--json", "usage: hgs swarm assign-launch --json");
            Ok(launch_project::execute(config, input()?))
        }
        "preference" => preference(config, &args[1..]),
        "worker" => { transport::worker()?; Ok(Value::Null) }
        "hello" | "inventory" => transport::hello(config,action == "inventory"),
        "preview" => transport::preview(config,alias()?),
        "bind" => transport::bind(config,alias()?),
        "join" => transport::join(config,alias()?,serde_json::from_value(input()?)?),
        "exchange" => transport::exchange(config,serde_json::from_value(input()?)?),
        "sync" => transport::sync(config),
        "get" | "initialize" | "apply" | "export" | "resolve" | "disconnect" => {
            // Read stdin before taking the lock: slow clients cannot monopolize it.
            let request = if matches!(action,"initialize"|"apply"|"resolve") { input()? } else { Value::Null };
            let mut store = LockedStore::load(config)?;
            let previous: std::collections::BTreeSet<_> = store.data.catalog.operations.keys().cloned().collect();
            match action {
                "initialize" => {
                    if !store.data.imported { store.backup("before-import")?; }
                    store.data.initialize(&request)?;
                }
                "apply" => { ensure!(store.data.imported,"initialize the local project catalog first"); store.data.apply(serde_json::from_value(request)?)?; }
                "resolve" => {
                    let key: catalog::Key = serde_json::from_str(request["key"].as_str().unwrap_or_default())?;
                    let versions: Vec<String> = serde_json::from_value(request["versions"].clone())?;
                    let actor = store.data.node_id.clone();
                    store.data.catalog.write(&actor,key,request["value"].clone(),versions)?;
                }
                "disconnect" => { store.data.peers.remove(alias()?); }
                _ => {}
            }
            store.save()?;
            if action == "export" { return Ok(json!({"schema":1,"node_id":store.data.node_id,"initialized":store.data.imported,"catalog":store.data.catalog})); }
            let mut snapshot = store.snapshot(config);
            snapshot["worker"] = worker.unwrap_or_else(|| service::status(config)).json();
            let mut written: std::collections::BTreeMap<String,Vec<String>> = std::collections::BTreeMap::new();
            for op in store.data.catalog.operations.values().filter(|op| !previous.contains(&op.id)) {
                written.entry(store.data.catalog.canonical_key(&op.key).token()).or_default().push(op.id.clone());
            }
            snapshot["written"] = json!(written);
            Ok(snapshot)
        }
        _ => bail!("usage: hgs swarm get | initialize | apply | preview PEER | join PEER | sync | worker | resolve | disconnect PEER | preference get|set"),
    }
}
