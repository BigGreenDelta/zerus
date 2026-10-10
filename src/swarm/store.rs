use super::catalog::{new_id, Catalog, Key};
use crate::config::Config;
use anyhow::{ensure, Context, Result};
use fs2::FileExt;
use serde::{Deserialize, Serialize};
use serde_json::json;
use std::{
    collections::BTreeMap,
    fs::{self, File, OpenOptions},
    io::Write,
    os::unix::fs::{OpenOptionsExt, PermissionsExt},
    path::PathBuf,
};

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
    /// The member runs an hgs without shared preferences; it keeps syncing
    /// projects and folders but does not receive the delivery choice.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub catalog_outdated: bool,
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
    pub path: PathBuf,
    _lock: File,
    original: Vec<u8>,
}

impl LockedStore {
    pub fn load(config: &Config) -> Result<Self> {
        let dir = config.dir.join("swarm");
        fs::create_dir_all(&dir)?;
        fs::set_permissions(&dir, fs::Permissions::from_mode(0o700))?;
        let lock = OpenOptions::new()
            .create(true)
            .truncate(false)
            .read(true)
            .write(true)
            .mode(0o600)
            .open(dir.join("catalog.lock"))?;
        lock.lock_exclusive()?;
        let path = dir.join("catalog.json");
        let original = match fs::read(&path) {
            Ok(v) => v,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => vec![],
            Err(e) => return Err(e.into()),
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
        let mut loaded = Self {
            data,
            path,
            _lock: lock,
            original,
        };
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
        Ok(loaded)
    }
    pub fn save(&mut self) -> Result<()> {
        self.data.catalog.validate()?;
        let bytes = serde_json::to_vec(&self.data)?;
        if bytes == self.original {
            return Ok(());
        }
        let dir = self.path.parent().unwrap();
        let mut temporary = tempfile::NamedTempFile::new_in(dir)?;
        temporary
            .as_file()
            .set_permissions(fs::Permissions::from_mode(0o600))?;
        temporary.write_all(&bytes)?;
        temporary.as_file().sync_all()?;
        temporary.persist(&self.path)?;
        File::open(dir)?.sync_all()?;
        self.original = bytes;
        Ok(())
    }
    pub fn backup(&self, reason: &str) -> Result<PathBuf> {
        let path = self
            .path
            .with_file_name(format!("backup-{reason}-{}.json", new_id()));
        let mut file = OpenOptions::new()
            .create_new(true)
            .write(true)
            .mode(0o600)
            .open(&path)?;
        file.write_all(&serde_json::to_vec(&self.data)?)?;
        file.sync_all()?;
        Ok(path)
    }
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
