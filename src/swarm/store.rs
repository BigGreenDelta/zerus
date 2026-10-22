use super::catalog::{new_id, Catalog, Key};
use crate::{config::Config, platform};
use anyhow::{ensure, Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{
    collections::BTreeMap,
    fs::{self, OpenOptions},
    io::{Read, Write},
    path::{Path, PathBuf},
};

/// Members whose hgs predates shared preferences. Kept beside catalog.json,
/// which an older hgs on this machine must still be able to read.
const OUTDATED: &str = "outdated-peers.json";

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Binding {
    pub id: String,
    pub verified: bool,
}
#[derive(Clone, Debug, Default, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Peer {
    pub node: String,
    #[serde(default)]
    pub last_sync: u64,
    #[serde(default)]
    pub error: String,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Store {
    pub node_id: String,
    pub catalog: Catalog,
    #[serde(default)]
    pub imported: bool,
    #[serde(default)]
    pub bindings: BTreeMap<String, Binding>,
    #[serde(default)]
    pub peers: BTreeMap<String, Peer>,
}

pub struct LockedStore {
    pub data: Store,
    /// Alias -> node of members that keep syncing projects and folders but do
    /// not receive the delivery choice. A missing or damaged file means none.
    pub outdated: BTreeMap<String, String>,
    pub path: PathBuf,
    _lock: platform::FileLock,
    original: Vec<u8>,
    outdated_original: Vec<u8>,
}

impl LockedStore {
    pub fn load(config: &Config) -> Result<Self> {
        Self::load_with(config, false, false)
    }
    /// An existing catalog for one scoped metadata operation. Do not stage the
    /// ordinary loader's machine-name/binding refresh as another shared edit.
    pub fn load_scoped(config: &Config) -> Result<Self> {
        Self::load_with(config, true, false)
    }
    /// Optional launch metadata must not wait behind a sync or user edit.
    pub fn try_load_scoped(config: &Config) -> Result<Self> {
        Self::load_with(config, true, true)
    }
    fn load_with(config: &Config, scoped: bool, nonblocking: bool) -> Result<Self> {
        let dir = config.dir.join("swarm");
        if scoped {
            ensure!(
                dir.is_dir(),
                "Shared projects are not initialized on this computer"
            );
        } else {
            fs::create_dir_all(&dir)?;
            platform::restrict_dir(&dir)?;
        }
        let lock = platform::private_open(
            OpenOptions::new()
                .create(!scoped)
                .truncate(false)
                .read(true)
                .write(true),
        )
        .open(dir.join("catalog.lock"))?;
        let lock = if nonblocking {
            fs2::FileExt::try_lock_exclusive(&lock)?;
            platform::FileLock::held(lock)
        } else {
            platform::FileLock::exclusive(lock)?
        };
        let path = dir.join("catalog.json");
        let original = if scoped {
            let metadata = fs::symlink_metadata(&path)?;
            ensure!(
                metadata.file_type().is_file(),
                "Shared project catalog is not a regular file"
            );
            let mut bytes = Vec::new();
            fs::File::open(&path)?
                .take(128 * 1024 * 1024 + 1)
                .read_to_end(&mut bytes)?;
            ensure!(
                bytes.len() <= 128 * 1024 * 1024,
                "Shared project catalog is too large"
            );
            ensure!(!bytes.is_empty(), "Shared project catalog is empty");
            bytes
        } else {
            match fs::read(&path) {
                Ok(v) => v,
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => vec![],
                Err(e) => return Err(e.into()),
            }
        };
        let data: Store = if original.is_empty() {
            Store {
                node_id: new_id(),
                catalog: Catalog::default(),
                imported: false,
                bindings: BTreeMap::new(),
                peers: BTreeMap::new(),
            }
        } else {
            serde_json::from_slice(&original)
                .context("read swarm catalog; the original file was left untouched")?
        };
        data.catalog.validate()?;
        let outdated_original = if scoped {
            vec![]
        } else {
            fs::read(dir.join(OUTDATED)).unwrap_or_default()
        };
        let mut loaded = Self {
            data,
            outdated: serde_json::from_slice(&outdated_original).unwrap_or_default(),
            path,
            _lock: lock,
            original,
            outdated_original,
        };
        if !scoped {
            let node = loaded.data.node_id.clone();
            for alias in &config.selves {
                loaded.data.bindings.insert(
                    alias.clone(),
                    Binding {
                        id: node.clone(),
                        verified: true,
                    },
                );
            }
            loaded
                .data
                .catalog
                .set(&node, Key::machine(&node, "name"), json!(config.host()))?;
        }
        Ok(loaded)
    }
    pub fn save(&mut self) -> Result<()> {
        self.save_catalog_only()?;
        let peers = &self.data.peers;
        self.outdated
            .retain(|alias, node| peers.get(alias).is_some_and(|p| &p.node == node));
        let outdated = if self.outdated.is_empty() {
            vec![]
        } else {
            serde_json::to_vec(&self.outdated)?
        };
        if outdated != self.outdated_original {
            let path = self.path.with_file_name(OUTDATED);
            if outdated.is_empty() {
                match fs::remove_file(path) {
                    Err(e) if e.kind() != std::io::ErrorKind::NotFound => return Err(e.into()),
                    _ => {}
                }
            } else {
                replace(&path, &outdated)?;
            }
            self.outdated_original = outdated;
        }
        Ok(())
    }
    /// Persist only the catalog. Scoped assignment must not prune peer metadata.
    pub fn save_catalog_only(&mut self) -> Result<()> {
        self.data.catalog.validate()?;
        let bytes = serde_json::to_vec(&self.data)?;
        if bytes != self.original {
            replace(&self.path, &bytes)?;
            self.original = bytes;
        }
        Ok(())
    }
    /// The catalog snapshot, naming members that need an hgs update.
    pub fn snapshot(&self, config: &Config) -> Value {
        let mut snapshot = self.data.snapshot(config);
        for (alias, peer) in &self.data.peers {
            if self.outdated.get(alias) == Some(&peer.node) {
                snapshot["peers"][alias]["catalog_outdated"] = json!(true);
            }
        }
        snapshot
    }
    pub fn backup(&self, reason: &str) -> Result<PathBuf> {
        let path = self
            .path
            .with_file_name(format!("backup-{reason}-{}.json", new_id()));
        let mut file =
            platform::private_open(OpenOptions::new().create_new(true).write(true)).open(&path)?;
        file.write_all(&serde_json::to_vec(&self.data)?)?;
        file.sync_all()?;
        Ok(path)
    }
}

pub(crate) fn replace(path: &Path, bytes: &[u8]) -> Result<()> {
    let dir = path.parent().unwrap();
    let mut temporary = tempfile::NamedTempFile::new_in(dir)?;
    platform::make_private(temporary.as_file())?;
    temporary.write_all(bytes)?;
    temporary.as_file().sync_all()?;
    platform::persist(temporary, path)?;
    platform::sync_dir(dir)?;
    Ok(())
}

impl Store {
    pub fn machine_id(&mut self, alias: &str) -> Result<String> {
        if super::catalog::uuid(alias) {
            return Ok(self.catalog.machine(alias));
        }
        ensure!(
            !alias.is_empty() && alias.len() <= 320 && !alias.chars().any(char::is_control),
            "invalid machine name"
        );
        if let Some(binding) = self.bindings.get(alias) {
            return Ok(self.catalog.machine(&binding.id));
        }
        let id = new_id();
        self.bindings.insert(
            alias.into(),
            Binding {
                id: id.clone(),
                verified: false,
            },
        );
        self.catalog
            .set(&self.node_id, Key::machine(&id, "name"), json!(alias))?;
        Ok(id)
    }
    pub fn bind(&mut self, alias: &str, id: &str) -> Result<()> {
        ensure!(super::catalog::uuid(id), "invalid remote machine ID");
        if let Some(old) = self.bindings.get(alias).cloned() {
            let old_id = self.catalog.machine(&old.id);
            ensure!(!old.verified || old_id == id, "the machine behind this connection changed; use a new local connection alias for a different machine");
            if old_id != id {
                self.catalog
                    .set(&self.node_id, Key::machine(&old_id, "redirect"), json!(id))?;
            }
        }
        self.bindings.insert(
            alias.into(),
            Binding {
                id: id.into(),
                verified: true,
            },
        );
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn scoped_session_edit_preserves_other_catalog_and_local_peer_metadata() {
        let dir = tempfile::tempdir().unwrap();
        let mut config = Config {
            home: dir.path().to_str().unwrap().into(),
            dir: dir.path().join("config"),
            state: dir.path().join("state"),
            selves: vec!["example-local".into()],
            peers: vec![],
            base_peers: vec![],
            peers_from_environment: false,
            machines: BTreeMap::new(),
            tab: false,
            tab_colors: String::new(),
        };
        let (catalog_path, node) = {
            let mut store = LockedStore::load(&config).unwrap();
            store.data.imported = true;
            store.save().unwrap();
            (store.path.clone(), store.data.node_id.clone())
        };
        let original: Value = serde_json::from_slice(&fs::read(&catalog_path).unwrap()).unwrap();
        let metadata = catalog_path.with_file_name(OUTDATED);
        fs::write(&metadata, b"{\"example-peer\":\"retained-metadata\"}").unwrap();
        let metadata_before = fs::read(&metadata).unwrap();
        config.selves = vec!["renamed-local".into()];
        let mut store = LockedStore::load_scoped(&config).unwrap();
        assert_eq!(serde_json::to_value(&store.data).unwrap(), original);
        let key = Key::Session {
            machine: node.clone(),
            session: "claude/example/task".into(),
        };
        store
            .data
            .catalog
            .set(&node, key.clone(), json!("logical"))
            .unwrap();
        store.save_catalog_only().unwrap();
        assert_eq!(fs::read(&metadata).unwrap(), metadata_before);
        let after: Value = serde_json::from_slice(&fs::read(&catalog_path).unwrap()).unwrap();
        for field in ["node_id", "bindings", "peers", "imported"] {
            assert_eq!(after[field], original[field]);
        }
        let added: Vec<_> = store
            .data
            .catalog
            .operations
            .iter()
            .filter(|(id, _)| original["catalog"]["operations"].get(id.as_str()).is_none())
            .collect();
        assert_eq!(added.len(), 1);
        assert_eq!(added[0].1.key, key);
    }

    #[test]
    fn outdated_members_stay_out_of_the_catalog_older_hgs_reads() {
        let dir = tempfile::tempdir().unwrap();
        let config = Config {
            home: dir.path().to_str().unwrap().into(),
            dir: dir.path().join("config"),
            state: dir.path().join("state"),
            selves: vec!["example-local".into()],
            peers: vec!["older".into(), "newer".into()],
            base_peers: vec![],
            peers_from_environment: false,
            machines: BTreeMap::new(),
            tab: false,
            tab_colors: String::new(),
        };
        let (older, newer) = (new_id(), new_id());
        let mut store = LockedStore::load(&config).unwrap();
        for (alias, node) in [("older", &older), ("newer", &newer)] {
            store.data.peers.insert(
                alias.into(),
                Peer {
                    node: node.clone(),
                    ..Peer::default()
                },
            );
        }
        store.outdated.insert("older".into(), older.clone());
        store.save().unwrap();
        drop(store);
        // The released Store and Peer definitions reject unknown fields.
        let stored: Value =
            serde_json::from_slice(&fs::read(config.dir.join("swarm/catalog.json")).unwrap())
                .unwrap();
        for peer in stored["peers"].as_object().unwrap().values() {
            let fields: Vec<_> = peer.as_object().unwrap().keys().cloned().collect();
            assert_eq!(fields, ["error", "last_sync", "node"]);
        }
        let store = LockedStore::load(&config).unwrap();
        let peers = &store.snapshot(&config)["peers"];
        assert_eq!(peers["older"]["catalog_outdated"], true);
        assert!(peers["newer"].get("catalog_outdated").is_none());
        drop(store);
        // A damaged file means no outdated member and is removed on save.
        let side = config.dir.join("swarm/outdated-peers.json");
        fs::write(&side, b"{damaged").unwrap();
        let mut store = LockedStore::load(&config).unwrap();
        assert!(store.snapshot(&config)["peers"]["older"]
            .get("catalog_outdated")
            .is_none());
        store.save().unwrap();
        assert!(!side.exists());
        // Entries for removed or replaced members are dropped.
        store.outdated.insert("older".into(), older);
        store.outdated.insert("newer".into(), new_id());
        store.data.peers.remove("older");
        store.save().unwrap();
        assert!(store.outdated.is_empty());
        assert!(!side.exists());
    }
}
