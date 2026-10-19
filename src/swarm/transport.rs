use super::{
    catalog::{self, Catalog, Key, Operation},
    replica, service,
    store::{LockedStore, Peer},
};
use crate::{config::Config, platform};
use anyhow::{ensure, Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{
    collections::{BTreeMap, BTreeSet},
    io::{Read, Write},
    thread,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

pub const MAX_BYTES: u64 = 32 * 1024 * 1024;
pub fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Exchange {
    pub swarm_id: String,
    pub known: BTreeSet<String>,
    pub operations: Vec<Operation>,
    /// Key kinds the requester parses. Older members neither send nor accept
    /// this field; responses to them contain only legacy kinds.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub kinds: Option<BTreeSet<String>>,
}

pub fn remote(config: &Config, alias: &str, action: &str, input: Option<&Value>) -> Result<Value> {
    run(config, alias, &["swarm", action], input)
}

/// `hgs WORDS…` on the member behind `alias`, with `input` as JSON on its
/// standard input; its JSON answer.
pub fn run(config: &Config, alias: &str, words: &[&str], input: Option<&Value>) -> Result<Value> {
    ensure!(
        config.peers.iter().any(|p| p == alias) && !config.is_self(alias),
        "choose a locally configured remote machine"
    );
    ensure!(
        !alias.starts_with('-') && !alias.chars().any(char::is_whitespace),
        "invalid connection alias"
    );
    let bytes = serde_json::to_vec(&input.unwrap_or(&json!({})))?;
    ensure!(
        bytes.len() as u64 <= MAX_BYTES,
        "swarm exchange is too large"
    );
    let mut command = platform::ssh();
    command
        .args([
            "-o",
            "BatchMode=yes",
            "-o",
            "ConnectTimeout=5",
            "-o",
            "ServerAliveInterval=5",
            "-o",
            "ServerAliveCountMax=2",
        ])
        .args(crate::machines::ssh_options(config, alias))
        .arg(alias)
        .arg(format!(
            "~/.local/bin/hgs {}",
            words.iter().map(|word| platform::quote(word)).collect::<Vec<_>>().join(" ")
        ));
    let ssh = platform::spawn_ssh(&mut command, true, true, true).context("start swarm connection")?;
    let mut child = ssh.child;
    let mut stdin = ssh.stdin.unwrap();
    let mut stdout = ssh.stdout.unwrap();
    let mut stderr = ssh.stderr.unwrap();
    let writer = thread::spawn(move || stdin.write_all(&bytes));
    let reader = thread::spawn(move || {
        let mut bytes = Vec::new();
        stdout
            .by_ref()
            .take(MAX_BYTES + 1)
            .read_to_end(&mut bytes)
            .map(|_| bytes)
    });
    let errors = thread::spawn(move || {
        let mut bytes = Vec::new();
        stderr
            .by_ref()
            .take(65536)
            .read_to_end(&mut bytes)
            .map(|_| bytes)
    });
    let started = Instant::now();
    let status = loop {
        if let Some(status) = child.try_wait()? {
            break Some(status);
        }
        if started.elapsed() > Duration::from_secs(25) {
            let _ = child.kill();
            let _ = child.wait();
            break None;
        }
        thread::sleep(Duration::from_millis(25));
    };
    let _ = writer.join();
    let output = reader
        .join()
        .map_err(|_| anyhow::anyhow!("swarm output reader failed"))??;
    let errors = errors
        .join()
        .map_err(|_| anyhow::anyhow!("swarm error reader failed"))??;
    ensure!(status.is_some(), "connection to {alias} timed out");
    ensure!(
        status.unwrap().success(),
        "{alias}: {}",
        String::from_utf8_lossy(&errors).trim()
    );
    ensure!(
        output.len() as u64 <= MAX_BYTES,
        "remote catalog is too large"
    );
    serde_json::from_slice(&output)
        .context("remote HGS does not provide a valid swarm response; update HGS on that machine")
}

pub fn hello(config: &Config, inventory: bool) -> Result<Value> {
    let mut store = LockedStore::load(config)?;
    store.save()?;
    let mut result = json!({"schema":1,"node_id":store.data.node_id,"swarm_id":store.data.catalog.swarm_id,"digest":store.data.catalog.digest(),"initialized":store.data.imported,"kinds":catalog::kinds()});
    if inventory {
        result["known"] = json!(store.data.catalog.operations.keys().collect::<Vec<_>>());
    }
    // Project stores are opened after the catalog lock is released. Tasks
    // that cannot be read never stop the catalog.
    drop(store);
    match replica::hello(config) {
        Ok(replica) => result["replica"] = replica,
        Err(error) => eprintln!("hgs swarm: tasks: {error:#}"),
    }
    Ok(result)
}

pub fn exchange(config: &Config, request: Exchange) -> Result<Value> {
    let mut store = LockedStore::load(config)?;
    store
        .data
        .catalog
        .merge(&request.swarm_id, request.operations)?;
    let missing: Vec<_> = store
        .data
        .catalog
        .operations
        .values()
        .filter(|op| {
            !request.known.contains(&op.id) && catalog::accepted(request.kinds.as_ref(), &op.key)
        })
        .cloned()
        .collect();
    store.save()?;
    Ok(
        json!({"schema":1,"node_id":store.data.node_id,"swarm_id":store.data.catalog.swarm_id,"operations":missing}),
    )
}

pub fn sync_peer(config: &Config, alias: &str) -> Result<()> {
    let identity = remote(config, alias, "hello", None)?;
    let mut local = LockedStore::load(config)?;
    let peer = local
        .data
        .peers
        .get(alias)
        .ok_or_else(|| anyhow::anyhow!("machine is not enrolled in this swarm"))?;
    ensure!(
        identity["schema"] == 1 && identity["node_id"] == peer.node,
        "the machine behind {alias} changed identity"
    );
    ensure!(
        identity["swarm_id"] == local.data.catalog.swarm_id,
        "{alias} belongs to a different swarm"
    );
    local
        .data
        .bind(alias, identity["node_id"].as_str().unwrap())?;
    // Older members reject unknown catalog kinds. They keep receiving the kinds
    // they parse; Settings names them until they are updated.
    let peer_kinds: Option<BTreeSet<String>> =
        serde_json::from_value(identity["kinds"].clone()).ok();
    if peer_kinds
        .as_ref()
        .is_some_and(|kinds| kinds.contains("preference"))
    {
        local.outdated.remove(alias);
    } else {
        let node = identity["node_id"].as_str().unwrap().to_owned();
        local.outdated.insert(alias.into(), node);
    }
    local.save()?;
    if identity["digest"] != local.data.catalog.digest() {
        drop(local);
        let inventory = remote(config, alias, "inventory", None)?;
        let local = LockedStore::load(config)?;
        let peer = local
            .data
            .peers
            .get(alias)
            .ok_or_else(|| anyhow::anyhow!("swarm connection was removed during sync"))?;
        ensure!(
            inventory["node_id"] == peer.node
                && inventory["swarm_id"] == local.data.catalog.swarm_id,
            "remote identity changed during sync"
        );
        let known: BTreeSet<String> = serde_json::from_value(inventory["known"].clone())?;
        let request = Exchange {
            swarm_id: local.data.catalog.swarm_id.clone(),
            known: local.data.catalog.operations.keys().cloned().collect(),
            operations: local
                .data
                .catalog
                .operations
                .values()
                .filter(|op| {
                    !known.contains(&op.id) && catalog::accepted(peer_kinds.as_ref(), &op.key)
                })
                .cloned()
                .collect(),
            kinds: peer_kinds.as_ref().map(|_| catalog::kinds()),
        };
        let node = peer.node.clone();
        drop(local);
        let response = remote(
            config,
            alias,
            "exchange",
            Some(&serde_json::to_value(request)?),
        )?;
        ensure!(
            response["node_id"] == node,
            "remote identity changed during exchange"
        );
        let mut local = LockedStore::load(config)?;
        ensure!(
            local.data.peers.get(alias).is_some_and(|p| p.node == node),
            "swarm connection changed during exchange"
        );
        local.data.catalog.merge(
            response["swarm_id"].as_str().unwrap_or_default(),
            serde_json::from_value(response["operations"].clone())?,
        )?;
        local.save()?;
    } else {
        drop(local);
    }
    replica::sync_peer(config, alias, &identity)?;
    let mut local = LockedStore::load(config)?;
    if let Some(peer) = local.data.peers.get_mut(alias) {
        peer.last_sync = now();
        peer.error.clear();
    }
    local.save()
}

pub fn sync(config: &Config) -> Result<Value> {
    let mut local = LockedStore::load(config)?;
    local.save()?;
    let peers: Vec<_> = local.data.peers.keys().cloned().collect();
    drop(local);
    let mut result = BTreeMap::new();
    for alias in peers {
        match sync_peer(config, &alias) {
            Ok(()) => {
                result.insert(alias, json!({"ok":true}));
            }
            Err(error) => {
                let message = format!("{error:#}");
                let mut store = LockedStore::load(config)?;
                if let Some(peer) = store.data.peers.get_mut(&alias) {
                    peer.error = message.clone();
                }
                store.save()?;
                result.insert(alias, json!({"ok":false,"error":message}));
            }
        }
    }
    Ok(json!(result))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Join {
    pub node_id: String,
    pub swarm_id: String,
    #[serde(default)]
    pub project_map: BTreeMap<String, String>,
    #[serde(default)]
    pub prefer_peer_memberships: bool,
}

pub fn bind(config: &Config, alias: &str) -> Result<Value> {
    let identity = remote(config, alias, "hello", None)?;
    ensure!(
        identity["schema"] == 1,
        "unsupported remote catalog protocol"
    );
    let mut local = LockedStore::load(config)?;
    local
        .data
        .bind(alias, identity["node_id"].as_str().unwrap_or_default())?;
    local.save()?;
    Ok(local.snapshot(config))
}

pub fn preview(config: &Config, alias: &str) -> Result<Value> {
    let remote = remote(config, alias, "export", None)?;
    let catalog: Catalog = serde_json::from_value(remote["catalog"].clone())?;
    catalog.validate()?;
    let mut local = LockedStore::load(config)?;
    local.save()?;
    let values = catalog.values();
    let projects: Vec<_> = values
        .iter()
        .filter_map(|(key, value)| match key {
            Key::Project { id, field } if field == "alive" && value == true => {
                Some(json!({"id":id,"name":values.get(&Key::project(id,"name"))}))
            }
            _ => None,
        })
        .collect();
    Ok(
        json!({"node_id":remote["node_id"],"swarm_id":catalog.swarm_id,"same_swarm":local.data.catalog.swarm_id == catalog.swarm_id,"projects":projects,"initialized":remote["initialized"]}),
    )
}

pub fn join(config: &Config, alias: &str, request: Join) -> Result<Value> {
    let remote = remote(config, alias, "export", None)?;
    ensure!(
        remote["node_id"] == request.node_id,
        "machine identity changed since preview; preview again"
    );
    let catalog: Catalog = serde_json::from_value(remote["catalog"].clone())?;
    catalog.validate()?;
    ensure!(
        catalog.swarm_id == request.swarm_id,
        "swarm changed since preview; preview again"
    );
    ensure!(
        remote["initialized"] == true,
        "open Projects in Zerus on the other machine first to import its existing catalog"
    );
    let mut local = LockedStore::load(config)?;
    ensure!(
        local.data.imported,
        "open Projects in Zerus on this machine first to import its existing catalog"
    );
    ensure!(
        request.node_id != local.data.node_id,
        "cannot join this machine to itself"
    );
    let same = local.data.catalog.swarm_id == catalog.swarm_id;
    ensure!(
        same || local.data.peers.is_empty(),
        "already enrolled in another swarm; merging entire swarms needs a separate migration"
    );
    // Normalize existing session identities before the initial membership merge.
    local.data.bind(alias, &request.node_id)?;
    if !same {
        let old = local.data.catalog.values();
        let remote_values = catalog.values();
        for (from, to) in &request.project_map {
            ensure!(
                old.get(&Key::project(from, "alive")) == Some(&json!(true))
                    && remote_values.get(&Key::project(to, "alive")) == Some(&json!(true)),
                "project mapping no longer matches both catalogs"
            );
        }
        local.backup("before-join")?;
        local.data.catalog = catalog;
        let mut existing = local.data.catalog.values();
        for (mut key, mut value) in old {
            // The swarm's delivery choice and project order win over the
            // joining machine's own.
            if matches!(key, Key::Preference { .. } | Key::Order { .. })
                && existing.contains_key(&key)
            {
                continue;
            }
            if request.prefer_peer_memberships
                && matches!(key, Key::Session { .. })
                && existing.get(&key).is_some_and(|v| !v.is_null())
            {
                continue;
            }
            match &mut key {
                Key::Project { id, .. } => {
                    if let Some(target) = request.project_map.get(id) {
                        *id = target.clone();
                    }
                }
                Key::Folder { .. } if !value.is_null() => {
                    if let Some(target) = value["project"]
                        .as_str()
                        .and_then(|id| request.project_map.get(id))
                    {
                        value["project"] = json!(target);
                    }
                }
                Key::Session { .. } => {
                    if let Some(target) = value.as_str().and_then(|id| request.project_map.get(id))
                    {
                        value = json!(target);
                    }
                }
                Key::Order { .. } => {
                    // Mapped projects are already in the swarm's catalog; keep
                    // the first place of each.
                    let mut seen = BTreeSet::new();
                    let ids = value
                        .as_array()
                        .into_iter()
                        .flatten()
                        .filter_map(Value::as_str);
                    value = ids
                        .map(|id| request.project_map.get(id).map_or(id, String::as_str))
                        .filter(|id| seen.insert(id.to_owned()))
                        .map(|id| json!(id))
                        .collect();
                }
                _ => {}
            }
            if existing.get(&key) != Some(&value) {
                existing.insert(key.clone(), value.clone());
                let actor = local.data.node_id.clone();
                local.data.catalog.write(&actor, key, value, vec![])?;
            }
        }
    } else {
        local.data.catalog.merge(
            &catalog.swarm_id,
            catalog.operations.into_values().collect(),
        )?;
    }
    local.data.bind(alias, &request.node_id)?;
    local.data.peers.insert(
        alias.into(),
        Peer {
            node: request.node_id,
            ..Peer::default()
        },
    );
    local.save()?;
    drop(local);
    // Enrollment is durable even if the final exchange is interrupted. The
    // worker retries it; do not misleadingly report the join as rolled back.
    let exchange = sync(config)?;
    let local = LockedStore::load(config)?;
    Ok(json!({"joined":true,"sync":exchange,"snapshot":local.snapshot(config)}))
}

pub fn worker() -> Result<()> {
    let config = Config::load()?;
    let executable = service::Executable::current();
    let mut initial = LockedStore::load(&config)?;
    initial.save()?;
    drop(initial);
    let lock = std::fs::OpenOptions::new()
        .create(true)
        .truncate(false)
        .write(true)
        .open(config.dir.join("swarm/worker.lock"))?;
    // Commands checking for a stale worker probe the lock for an instant.
    // Unlike the recovery and voice workers (state::service_worker), no client
    // starts this one detached: only the service runs it on Linux and macOS,
    // and the desktop on Windows. A lock held for longer belongs to a worker
    // started by hand.
    let mut attempts = 0;
    while let Err(error) = fs2::FileExt::try_lock_exclusive(&lock) {
        attempts += 1;
        if attempts == 20 {
            return Err(error).context("a swarm worker is already running");
        }
        thread::sleep(Duration::from_millis(100));
    }
    let _lock = crate::platform::FileLock::held(lock);
    if let Err(error) = service::record(&config) {
        eprintln!("hgs swarm: record the worker version: {error:#}");
    }
    let mut schedule: BTreeMap<String, (u64, u64)> = BTreeMap::new();
    let mut last_digest = String::new();
    loop {
        // An upgrade replaced this program: exit so that the service manager
        // starts the new version. Catalog errors never end the loop.
        ensure!(
            !executable.as_ref().is_some_and(service::Executable::replaced),
            "the hgs executable was replaced; exiting so that the new version starts"
        );
        match Config::load()
            .map_err(anyhow::Error::from)
            .and_then(|config| {
                let mut store = LockedStore::load(&config)?;
                store.save()?;
                let catalog = store.data.catalog.digest();
                let peers: Vec<_> = store.data.peers.keys().cloned().collect();
                drop(store);
                // A local change to the catalog or to any task syncs at once.
                let digest = catalog + &replica::fingerprint(&config).unwrap_or_default();
                let changed = digest != last_digest;
                for alias in peers {
                    let (due, failures) = schedule.get(&alias).copied().unwrap_or_default();
                    if now() < due && !(changed && failures == 0) {
                        continue;
                    }
                    match sync_peer(&config, &alias) {
                        Ok(()) => {
                            schedule.insert(alias, (now() + 30, 0));
                        }
                        Err(error) => {
                            let mut store = LockedStore::load(&config)?;
                            if let Some(peer) = store.data.peers.get_mut(&alias) {
                                peer.error = format!("{error:#}");
                            }
                            store.save()?;
                            let failures = (failures + 1).min(5);
                            schedule
                                .insert(alias, (now() + (15 * (1 << failures)).min(300), failures));
                        }
                    }
                }
                last_digest = LockedStore::load(&config)?.data.catalog.digest() + &replica::fingerprint(&config).unwrap_or_default();
                Ok(())
            }) {
            Ok(()) => {}
            Err(error) => eprintln!("hgs swarm: {error:#}"),
        }
        thread::sleep(Duration::from_secs(3));
    }
}
