//! Replicating a project between replicas: what each holds (its inventory, the
//! version of every document), the updates the other lacks, and importing them.
//! The inventory is bookkeeping, not a log, so nothing here grows without bound.
use crate::store::{write_atomic, Damaged, ProjectStore};
use crate::task::TaskDoc;
use crate::{ids, Error, Result};
use loro::VersionVector;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::fs;
use std::path::Path;
use std::time::UNIX_EPOCH;

/// A document's version: each Loro peer that changed it (its ID as decimal
/// text) and how many changes it made. Sorted, so equal versions compare and
/// hash alike on every replica.
pub type Version = BTreeMap<String, i32>;

/// What a replica holds of one project.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Inventory {
    /// The project document; empty while it was never saved.
    pub project: Version,
    /// Each task document by UUID.
    pub tasks: BTreeMap<String, Version>,
}

impl Inventory {
    /// Equal for replicas that hold the same versions of the same documents.
    pub fn digest(&self) -> String {
        let canonical = serde_json::to_vec(self).expect("an inventory serializes");
        format!("{:x}", Sha256::digest(canonical))
    }
}

/// The document an update belongs to.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Document {
    Project,
    Task(String),
}

/// Loro updates for one document: what the other replica lacks of it, or the
/// whole document when it has none.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Update {
    pub document: Document,
    pub bytes: Vec<u8>,
}

/// What an import changed, and the documents it refused.
#[derive(Debug, Default)]
pub struct Imported {
    pub changed: usize,
    pub refused: Vec<Damaged>,
}

const CACHE: &str = "inventory.json";

/// What tells a file changed without reading it: its length, modification
/// time and, on Unix, its inode (an atomic rewrite gets a new one).
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
struct Stamp(u64, u64, u32, u64);

fn stamp(path: &Path) -> Option<Stamp> {
    let meta = fs::metadata(path).ok()?;
    let modified = meta.modified().ok()?.duration_since(UNIX_EPOCH).ok()?;
    #[cfg(unix)]
    let inode = std::os::unix::fs::MetadataExt::ino(&meta);
    #[cfg(not(unix))]
    let inode = 0;
    Some(Stamp(
        meta.len(),
        modified.as_secs(),
        modified.subsec_nanos(),
        inode,
    ))
}

/// The versions read last time, by the stamps of the files they came from.
#[derive(Default, PartialEq, Serialize, Deserialize)]
struct Cache {
    project: Option<Cached>,
    tasks: BTreeMap<String, Cached>,
}

#[derive(Clone, PartialEq, Serialize, Deserialize)]
struct Cached {
    files: Vec<Option<Stamp>>,
    version: Version,
}

fn version(vv: &VersionVector) -> Version {
    vv.iter()
        .map(|(peer, count)| (peer.to_string(), *count))
        .collect()
}

fn vector(version: &Version) -> Result<VersionVector> {
    version
        .iter()
        .map(|(peer, count)| {
            let peer = peer
                .parse::<u64>()
                .map_err(|_| Error::new(format!("invalid peer {peer:?} in a version")))?;
            Ok((peer, *count))
        })
        .collect()
}

/// Whether `theirs` holds every change of `ours`.
fn includes(theirs: &Version, ours: &Version) -> bool {
    ours.iter()
        .all(|(peer, count)| theirs.get(peer).is_some_and(|known| known >= count))
}

impl ProjectStore {
    /// The project IDs stored below `root`.
    pub fn stored(root: &Path) -> Result<Vec<String>> {
        let entries = match fs::read_dir(root) {
            Ok(entries) => entries,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(vec![]),
            Err(error) => return Err(error.into()),
        };
        let mut out = Vec::new();
        for entry in entries {
            if let Ok(id) = fs::read_to_string(entry?.path().join("project-id")) {
                out.push(id);
            }
        }
        out.sort();
        Ok(out)
    }

    /// The version of every document of the project, and the tasks that do not
    /// load. A task is read again only when its files changed since last time.
    pub fn inventory(&self) -> Result<(Inventory, Vec<Damaged>)> {
        let path = self.dir().join(CACHE);
        let old: Cache = fs::read(&path)
            .ok()
            .and_then(|bytes| serde_json::from_slice(&bytes).ok())
            .unwrap_or_default();
        let mut cache = Cache::default();
        let mut inventory = Inventory::default();
        let mut damaged = Vec::new();
        let project = vec![stamp(&self.dir().join("project.loro"))];
        if project[0].is_some() {
            let cached = match old.project.as_ref().filter(|c| c.files == project) {
                Some(cached) => cached.clone(),
                None => Cached {
                    files: project,
                    version: version(&VersionVector::decode(&self.project()?.version())?),
                },
            };
            inventory.project = cached.version.clone();
            cache.project = Some(cached);
        }
        for uuid in self.uuids()? {
            let files = vec![
                stamp(&self.snapshot_path(&uuid)),
                stamp(&self.updates_path(&uuid)),
            ];
            let cached = match old.tasks.get(&uuid).filter(|c| c.files == files) {
                Some(cached) => cached.clone(),
                None => match self.load(&uuid) {
                    Ok(task) => Cached {
                        files,
                        version: version(&task.current_vv()),
                    },
                    Err(error) => {
                        damaged.push(Damaged {
                            uuid,
                            error: error.to_string(),
                        });
                        continue;
                    }
                },
            };
            inventory.tasks.insert(uuid.clone(), cached.version.clone());
            cache.tasks.insert(uuid, cached);
        }
        if cache != old {
            // Only a speed-up: an unwritten cache means reading the tasks again.
            let _ = write_atomic(&path, &serde_json::to_vec(&cache)?);
        }
        Ok((inventory, damaged))
    }

    /// The updates a replica holding `theirs` lacks, in pages of at most
    /// `limit` bytes; a document larger than that comes alone. True when more
    /// remain after this page.
    pub fn updates_for(&self, theirs: &Inventory, limit: usize) -> Result<(Vec<Update>, bool)> {
        let (ours, _) = self.inventory()?;
        let mut page = Vec::new();
        let mut size = 0;
        let mut add = |update: Update| {
            if !page.is_empty() && size + update.bytes.len() > limit {
                return false;
            }
            size += update.bytes.len();
            page.push(update);
            true
        };
        if !ours.project.is_empty() && !includes(&theirs.project, &ours.project) {
            let bytes = self
                .project()?
                .export_since(&vector(&theirs.project)?.encode())?;
            if !add(Update {
                document: Document::Project,
                bytes,
            }) {
                return Ok((page, true));
            }
        }
        for (uuid, mine) in &ours.tasks {
            let known = theirs.tasks.get(uuid);
            if known.is_some_and(|known| includes(known, mine)) {
                continue;
            }
            let task = self.load(uuid)?;
            let bytes = match known {
                Some(known) => task.export_since_vv(&vector(known)?)?,
                None => task.snapshot()?,
            };
            if !add(Update {
                document: Document::Task(uuid.clone()),
                bytes,
            }) {
                return Ok((page, true));
            }
        }
        Ok((page, false))
    }

    /// Imports another replica's updates. A document that cannot take them is
    /// refused and named; it never stops the others.
    pub fn import_updates(&self, updates: Vec<Update>) -> Result<Imported> {
        let mut imported = Imported::default();
        for update in updates {
            let (name, result) = match &update.document {
                Document::Project => ("project".to_owned(), self.import_project(&update.bytes)),
                Document::Task(uuid) => (uuid.clone(), self.import_task(uuid, &update.bytes)),
            };
            match result {
                Ok(changed) => imported.changed += usize::from(changed),
                Err(error) => imported.refused.push(Damaged {
                    uuid: name,
                    error: error.to_string(),
                }),
            }
        }
        Ok(imported)
    }

    fn import_project(&self, bytes: &[u8]) -> Result<bool> {
        let mut doc = self.project()?;
        let before = VersionVector::decode(&doc.version())?;
        doc.import(bytes)?;
        if VersionVector::decode(&doc.version())? == before {
            return Ok(false);
        }
        self.save_project(&doc)?;
        Ok(true)
    }

    fn import_task(&self, uuid: &str, bytes: &[u8]) -> Result<bool> {
        if !ids::is_uuid(uuid) {
            return Err(Error::new(format!("{uuid:?} is not a task UUID")));
        }
        let stored = self.snapshot_path(uuid).exists() || self.updates_path(uuid).exists();
        let mut task = if stored {
            let mut task = self.load(uuid)?;
            let before = task.current_vv();
            if !task.import_block(bytes) {
                return Err(Error::new("the updates could not be applied"));
            }
            if task.current_vv() == before {
                return Ok(false);
            }
            task
        } else {
            TaskDoc::from_bytes(bytes)?
        };
        if task.uuid() != uuid {
            return Err(Error::new("the update is for another task"));
        }
        let view = task.view()?;
        if view.project != self.project_id() {
            return Err(Error::new("the task belongs to another project"));
        }
        self.save(&mut task)?;
        Ok(true)
    }
}
