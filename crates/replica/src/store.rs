//! Documents on disk, one directory per project:
//! `<root>/<sha256(project ID)>/project.loro` and `tasks/<uuid>.loro`, plus
//! `tasks/<uuid>.updates` with the updates appended since that snapshot. Each
//! appended block carries its length and a checksum; a block cut short or
//! damaged by a crash ends the list, and the next save rewrites the snapshot.
//! Updates are folded into a fresh snapshot when they grow, and once when a
//! task is accepted (its Done compaction). One process at a time holds a project.
use crate::project::ProjectDoc;
use crate::schema::{Actor, Stage, SCHEMA};
use crate::task::TaskDoc;
use crate::{ids, rank, Error, Result};
use fs2::FileExt;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::BTreeSet;
use std::fs::{self, File, OpenOptions};
use std::io::{ErrorKind, Write};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

/// A rank longer than this renumbers its stage (ranks stay valid up to 64).
const RENUMBER_AT: usize = 32;
/// Appended updates beyond this size are folded into the snapshot.
const FOLD_BYTES: u64 = 256 * 1024;
/// A block: little-endian `u32` length, the first four bytes of the SHA-256
/// of the payload, then the payload (Loro updates).
const HEADER: usize = 8;

/// A task file that failed to load. It stays on disk and never blocks the others.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Damaged {
    pub uuid: String,
    pub error: String,
}

/// Where a task goes in the order of its stage.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Position {
    Top,
    End,
    /// Zero-based; beyond the end means the end.
    Index(usize),
    /// Before or after the task with this short ID or UUID.
    Before(String),
    After(String),
}

pub struct ProjectStore {
    dir: PathBuf,
    project: String,
    _lock: File,
}

fn private_options() -> OpenOptions {
    let mut options = OpenOptions::new();
    #[cfg(unix)]
    std::os::unix::fs::OpenOptionsExt::mode(&mut options, 0o600);
    options
}

fn private_dir(path: &Path) -> Result<()> {
    fs::create_dir_all(path)?;
    #[cfg(unix)]
    fs::set_permissions(path, std::os::unix::fs::PermissionsExt::from_mode(0o700))?;
    Ok(())
}

pub(crate) fn write_atomic(path: &Path, bytes: &[u8]) -> Result<()> {
    let temp = path.with_extension(format!("tmp-{}", ids::new_uuid()));
    let mut file = private_options().create_new(true).write(true).open(&temp)?;
    file.write_all(bytes)?;
    file.sync_all()?;
    fs::rename(&temp, path)?;
    // The rename itself survives a power loss only once the directory is synced.
    #[cfg(unix)]
    if let Some(parent) = path.parent() {
        File::open(parent)?.sync_all()?;
    }
    Ok(())
}

fn checksum(payload: &[u8]) -> [u8; 4] {
    Sha256::digest(payload)[..4].try_into().expect("four bytes")
}

fn block(payload: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(HEADER + payload.len());
    out.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    out.extend_from_slice(&checksum(payload));
    out.extend_from_slice(payload);
    out
}

/// The intact blocks with their end offsets, and whether the whole input was
/// intact. A short, empty or mismatched block ends the list.
fn blocks(bytes: &[u8]) -> (Vec<(&[u8], usize)>, bool) {
    let mut out = Vec::new();
    let mut at = 0;
    while at < bytes.len() {
        let rest = &bytes[at..];
        if rest.len() < HEADER {
            return (out, false);
        }
        let length = u32::from_le_bytes(rest[..4].try_into().expect("four bytes")) as usize;
        if length == 0 || rest.len() - HEADER < length {
            return (out, false);
        }
        let payload = &rest[HEADER..HEADER + length];
        if checksum(payload) != rest[4..8] {
            return (out, false);
        }
        at += HEADER + length;
        out.push((payload, at));
    }
    (out, true)
}

impl ProjectStore {
    /// Opens the project's directory below `root` and waits for its lock.
    pub fn open(root: &Path, project: &str) -> Result<Self> {
        Ok(Self::opening(root, project, None)?.expect("open waits for the lock"))
    }

    /// Opens the project like `open`, but gives up after `wait` while another
    /// process holds its lock.
    pub fn open_within(root: &Path, project: &str, wait: Duration) -> Result<Option<Self>> {
        Self::opening(root, project, Some(wait))
    }

    fn opening(root: &Path, project: &str, wait: Option<Duration>) -> Result<Option<Self>> {
        if project.is_empty() || project.len() > 128 || project.chars().any(char::is_control) {
            return Err(Error::new("invalid project ID"));
        }
        private_dir(root)?;
        let dir = root.join(format!("{:x}", Sha256::digest(project.as_bytes())));
        private_dir(&dir)?;
        private_dir(&dir.join("tasks"))?;
        let lock = private_options()
            .create(true)
            .truncate(false)
            .write(true)
            .open(dir.join(".lock"))?;
        match wait {
            None => lock.lock_exclusive()?,
            Some(wait) => {
                let deadline = Instant::now() + wait;
                while lock.try_lock_exclusive().is_err() {
                    if Instant::now() >= deadline {
                        return Ok(None);
                    }
                    std::thread::sleep(Duration::from_millis(20));
                }
            }
        }
        // A crash during an atomic write leaves its temporary file behind. No
        // one else writes while this lock is held, so any left now is garbage;
        // removing it is best effort.
        for folder in [dir.clone(), dir.join("tasks")] {
            for entry in fs::read_dir(&folder)? {
                let path = entry?.path();
                let temporary = path
                    .extension()
                    .and_then(|e| e.to_str())
                    .is_some_and(|e| e.starts_with("tmp-"));
                if temporary {
                    let _ = fs::remove_file(&path);
                }
            }
        }
        let name = dir.join("project-id");
        if !name.exists() {
            write_atomic(&name, project.as_bytes())?;
        }
        Ok(Some(Self {
            dir,
            project: project.into(),
            _lock: lock,
        }))
    }

    pub fn project_id(&self) -> &str {
        &self.project
    }

    pub(crate) fn dir(&self) -> &Path {
        &self.dir
    }

    pub fn project(&self) -> Result<ProjectDoc> {
        match fs::read(self.dir.join("project.loro")) {
            Ok(bytes) => ProjectDoc::from_bytes(&bytes),
            Err(error) if error.kind() == ErrorKind::NotFound => Ok(ProjectDoc::new()),
            Err(error) => Err(error.into()),
        }
    }

    pub fn save_project(&self, doc: &ProjectDoc) -> Result<()> {
        write_atomic(&self.dir.join("project.loro"), &doc.snapshot()?)
    }

    pub(crate) fn snapshot_path(&self, uuid: &str) -> PathBuf {
        self.dir.join("tasks").join(format!("{uuid}.loro"))
    }

    pub(crate) fn updates_path(&self, uuid: &str) -> PathBuf {
        self.dir.join("tasks").join(format!("{uuid}.updates"))
    }

    pub fn uuids(&self) -> Result<Vec<String>> {
        let mut uuids = BTreeSet::new();
        for entry in fs::read_dir(self.dir.join("tasks"))? {
            let path = entry?.path();
            let extension = path.extension().and_then(|e| e.to_str());
            if matches!(extension, Some("loro" | "updates")) {
                if let Some(stem) = path
                    .file_stem()
                    .and_then(|s| s.to_str())
                    .filter(|s| ids::is_uuid(s))
                {
                    uuids.insert(stem.to_owned());
                }
            }
        }
        Ok(uuids.into_iter().collect())
    }

    pub fn load(&self, uuid: &str) -> Result<TaskDoc> {
        let mut task = TaskDoc::from_bytes(&fs::read(self.snapshot_path(uuid))?)?;
        if task.uuid() != uuid {
            return Err(Error::failed("the task file does not match its name"));
        }
        match fs::read(self.updates_path(uuid)) {
            Ok(bytes) => {
                let (found, intact) = blocks(&bytes);
                task.needs_fold = !intact;
                for (payload, end) in found {
                    if !task.import_block(payload) {
                        task.needs_fold = true;
                        break;
                    }
                    task.saved_len = end as u64;
                }
            }
            Err(error) if error.kind() == ErrorKind::NotFound => {}
            Err(error) => return Err(error.into()),
        }
        // A task that cannot be shown is damaged: listed apart, never blocking the rest.
        task.view()?;
        task.saved = Some(task.current_vv());
        task.saved_stage = Some(task.stage()?);
        Ok(task)
    }

    pub fn save(&self, task: &mut TaskDoc) -> Result<()> {
        let snapshot = self.snapshot_path(task.uuid());
        let updates = self.updates_path(task.uuid());
        let current = task.current_vv();
        let stage = task.stage()?;
        let on_disk = match fs::metadata(&updates) {
            Ok(meta) => meta.len(),
            Err(error) if error.kind() == ErrorKind::NotFound => 0,
            Err(error) => return Err(error.into()),
        };
        let fold = task.saved.is_none()
            || !snapshot.exists()
            || task.needs_fold
            // Bytes past the valid blocks: a cut append, or another writer.
            || on_disk != task.saved_len
            || on_disk >= FOLD_BYTES
            || (stage == Stage::Done && task.saved_stage != Some(Stage::Done));
        if !fold && task.saved.as_ref() == Some(&current) {
            return Ok(());
        }
        if fold {
            // The snapshot first: a crash before the removal leaves updates
            // the snapshot already contains, and importing them again is harmless.
            write_atomic(&snapshot, &task.snapshot()?)?;
            match fs::remove_file(&updates) {
                Ok(()) => {}
                Err(error) if error.kind() == ErrorKind::NotFound => {}
                Err(error) => return Err(error.into()),
            }
            task.saved_len = 0;
            task.needs_fold = false;
        } else {
            let saved = task.saved.clone().expect("checked above");
            let bytes = block(&task.export_since_vv(&saved)?);
            let mut file = private_options().create(true).append(true).open(&updates)?;
            file.write_all(&bytes)?;
            file.sync_all()?;
            task.saved_len += bytes.len() as u64;
        }
        task.saved = Some(current);
        task.saved_stage = Some(stage);
        Ok(())
    }

    /// Every task that loads, and the ones that do not.
    pub fn tasks(&self) -> Result<(Vec<TaskDoc>, Vec<Damaged>)> {
        let mut tasks = Vec::new();
        let mut damaged = Vec::new();
        for uuid in self.uuids()? {
            match self.load(&uuid) {
                Ok(task) => tasks.push(task),
                Err(error) => damaged.push(Damaged {
                    uuid,
                    error: error.to_string(),
                }),
            }
        }
        Ok((tasks, damaged))
    }

    /// A task by its UUID or its short ID.
    pub fn find(&self, reference: &str) -> Result<TaskDoc> {
        if ids::is_uuid(reference) {
            if !self.snapshot_path(reference).exists() && !self.updates_path(reference).exists() {
                return Err(Error::new(format!("no task {reference} in this project")));
            }
            return self
                .load(reference)
                .map_err(|e| Error::failed(format!("task {reference} is damaged: {e}")));
        }
        if !ids::is_short_id(reference) {
            return Err(Error::new(format!("{reference:?} is not a task ID")));
        }
        let (tasks, damaged) = self.tasks()?;
        let mut found: Vec<_> = tasks.into_iter().filter(|t| t.id() == reference).collect();
        match found.len() {
            // A damaged file's short ID cannot be read: it may be the one.
            0 if !damaged.is_empty() => Err(Error::new(format!(
                "no task {reference} in this project; {} could not be read",
                match damaged.len() {
                    1 => "1 task file".to_owned(),
                    n => format!("{n} task files"),
                }
            ))),
            0 => Err(Error::new(format!("no task {reference} in this project"))),
            1 => Ok(found.pop().expect("one")),
            _ => Err(Error::new(format!(
                "{reference} names more than one task; use its UUID"
            ))),
        }
    }

    /// A short ID no local task uses yet.
    pub fn unused_short_id(&self) -> Result<String> {
        let (tasks, _) = self.tasks()?;
        let used: BTreeSet<String> = tasks.iter().map(TaskDoc::id).collect();
        loop {
            let id = ids::short_id();
            if !used.contains(&id) {
                return Ok(id);
            }
        }
    }

    /// The tasks of `stage` in their order: rank, then UUID for equal ranks.
    pub fn ordered(&self, stage: Stage) -> Result<Vec<TaskDoc>> {
        let (tasks, _) = self.listing()?;
        Ok(tasks
            .into_iter()
            .filter(|t| t.stage().ok() == Some(stage))
            .collect())
    }

    /// Every task, loaded once and sorted by rank then UUID (so each stage is
    /// in its order), and the ones that do not load.
    pub fn listing(&self) -> Result<(Vec<TaskDoc>, Vec<Damaged>)> {
        let (mut tasks, damaged) = self.tasks()?;
        tasks.sort_by(|a, b| (a.rank(), a.uuid()).cmp(&(b.rank(), b.uuid())));
        Ok((tasks, damaged))
    }

    /// The rank that puts a task at `position` in `stage`, leaving out the
    /// task being moved (its UUID).
    pub fn rank_for(
        &self,
        stage: Stage,
        position: &Position,
        moving: Option<&str>,
    ) -> Result<String> {
        let list: Vec<TaskDoc> = self
            .ordered(stage)?
            .into_iter()
            .filter(|t| Some(t.uuid()) != moving)
            .collect();
        let at = |reference: &str| {
            list.iter()
                .position(|t| t.id() == reference || t.uuid() == reference)
                .ok_or_else(|| Error::new(format!("{reference} is not in {}", stage.label())))
        };
        let index = match position {
            Position::Top => 0,
            Position::End => list.len(),
            Position::Index(index) => (*index).min(list.len()),
            Position::Before(reference) => at(reference)?,
            Position::After(reference) => at(reference)? + 1,
        };
        let low = index.checked_sub(1).map(|i| list[i].rank());
        // Equal ranks can meet after concurrent moves: aim below the first higher one.
        let high = list[index..]
            .iter()
            .map(TaskDoc::rank)
            .find(|r| low.as_ref().is_none_or(|l| r > l));
        let placed = rank::between(low.as_deref(), high.as_deref());
        if placed.as_ref().is_ok_and(|r| r.len() <= RENUMBER_AT)
            || list.iter().any(|t| t.schema() > SCHEMA)
        {
            return placed;
        }
        // Inserting at one place again and again lengthens ranks; renumber the
        // stage evenly, in its current order, and place the task among them.
        let fresh = rank::spread(list.len());
        for (mut task, rank) in list.into_iter().zip(&fresh) {
            if task.rank() != *rank {
                task.set_rank(rank)?;
                self.save(&mut task)?;
            }
        }
        let low = index.checked_sub(1).map(|i| fresh[i].as_str());
        rank::between(low, fresh.get(index).map(String::as_str))
    }

    /// How many tasks agents started since `since` (Unix milliseconds).
    pub fn agent_starts_since(&self, since: i64) -> Result<usize> {
        let (tasks, _) = self.tasks()?;
        let mut count = 0;
        for task in tasks {
            count += task
                .view()?
                .log
                .iter()
                .filter(|e| e.kind == "sent" && e.at >= since && matches!(e.actor, Actor::Agent(_)))
                .count();
        }
        Ok(count)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::schema::{SessionKey, Target};
    use crate::task::NewTask;
    use crate::ErrorKind;

    fn new_task(store: &ProjectStore, rank: &str) -> TaskDoc {
        TaskDoc::create(NewTask {
            project: store.project_id(),
            id: &store.unused_short_id().unwrap(),
            text: "Add rate limiting",
            stage: Stage::Todo,
            target: Target::Any,
            author: Actor::Person,
            rank,
            now: 1,
        })
        .unwrap()
    }

    fn api() -> SessionKey {
        SessionKey {
            machine: "m".into(),
            session: "codex/api".into(),
        }
    }

    fn comments(task: &TaskDoc) -> Vec<String> {
        task.view()
            .unwrap()
            .log
            .into_iter()
            .filter(|e| e.kind == "comment")
            .map(|e| e.text)
            .collect()
    }

    fn append(store: &ProjectStore, task: &TaskDoc, bytes: &[u8]) {
        let mut file = OpenOptions::new()
            .create(true)
            .append(true)
            .open(store.updates_path(task.uuid()))
            .unwrap();
        file.write_all(bytes).unwrap();
    }

    #[test]
    fn saves_append_updates_and_reload() {
        let root = tempfile::tempdir().unwrap();
        let store = ProjectStore::open(root.path(), "p1").unwrap();
        let mut task = new_task(&store, "V");
        store.save(&mut task).unwrap();
        task.comment("first", &Actor::Person, 2).unwrap();
        store.save(&mut task).unwrap();
        task.comment("second", &Actor::Person, 3).unwrap();
        store.save(&mut task).unwrap();
        let updates = fs::read(store.updates_path(task.uuid())).unwrap();
        let (found, intact) = blocks(&updates);
        assert_eq!((found.len(), intact), (2, true));
        let loaded = store.find(&task.id()).unwrap();
        assert_eq!(loaded.view().unwrap(), task.view().unwrap());
        assert_eq!(
            store.find(task.uuid()).unwrap().view().unwrap(),
            task.view().unwrap()
        );
    }

    #[test]
    fn a_cut_tail_is_repaired_and_later_saves_survive() {
        for junk in [&[200, 0, 0, 0, 1, 2][..], &[0; 64][..]] {
            let root = tempfile::tempdir().unwrap();
            let store = ProjectStore::open(root.path(), "p1").unwrap();
            let mut task = new_task(&store, "V");
            store.save(&mut task).unwrap();
            task.comment("first", &Actor::Person, 2).unwrap();
            store.save(&mut task).unwrap();
            append(&store, &task, junk);
            // A later process: the cut tail is skipped, and the next save rewrites the snapshot.
            let mut again = store.load(task.uuid()).unwrap();
            assert!(again.needs_fold);
            again.comment("after the crash", &Actor::Person, 3).unwrap();
            store.save(&mut again).unwrap();
            assert!(!store.updates_path(task.uuid()).exists());
            again.comment("then appended", &Actor::Person, 4).unwrap();
            store.save(&mut again).unwrap();
            let last = store.load(task.uuid()).unwrap();
            assert_eq!(
                comments(&last),
                ["first", "after the crash", "then appended"]
            );
            assert!(!last.needs_fold);
        }
    }

    #[test]
    fn a_damaged_block_ends_the_updates() {
        // A damaged payload byte, and a damaged checksum over an intact payload.
        for damaged in [|len: usize| len - 1, |_| 4] {
            let root = tempfile::tempdir().unwrap();
            let store = ProjectStore::open(root.path(), "p1").unwrap();
            let mut task = new_task(&store, "V");
            store.save(&mut task).unwrap();
            task.comment("first", &Actor::Person, 2).unwrap();
            store.save(&mut task).unwrap();
            let path = store.updates_path(task.uuid());
            let mut bytes = fs::read(&path).unwrap();
            let at = damaged(bytes.len());
            bytes[at] ^= 0xff;
            fs::write(&path, bytes).unwrap();
            let loaded = store.load(task.uuid()).unwrap();
            assert!(loaded.needs_fold);
            assert!(comments(&loaded).is_empty());
        }
    }

    #[test]
    fn done_compacts_once() {
        let root = tempfile::tempdir().unwrap();
        let store = ProjectStore::open(root.path(), "p1").unwrap();
        let mut task = new_task(&store, "V");
        store.save(&mut task).unwrap();
        task.record_send(&api(), "r", &Actor::Person, 2).unwrap();
        store.save(&mut task).unwrap();
        task.record_report("Done.", &Actor::Person, 3).unwrap();
        task.set_stage(Stage::Done, &Actor::Person, 4).unwrap();
        store.save(&mut task).unwrap();
        assert!(!store.updates_path(task.uuid()).exists());
        // Comments on a Done task are appended again.
        task.comment("thanks", &Actor::Person, 5).unwrap();
        store.save(&mut task).unwrap();
        assert!(store.updates_path(task.uuid()).exists());
        assert_eq!(
            store.load(task.uuid()).unwrap().view().unwrap(),
            task.view().unwrap()
        );
    }

    #[test]
    fn a_damaged_file_is_set_aside_and_projects_are_separate() {
        let root = tempfile::tempdir().unwrap();
        let store = ProjectStore::open(root.path(), "p1").unwrap();
        let mut task = new_task(&store, "V");
        store.save(&mut task).unwrap();
        let bad = ids::new_uuid();
        fs::write(store.snapshot_path(&bad), b"not loro").unwrap();
        let (tasks, damaged) = store.tasks().unwrap();
        assert_eq!(tasks.len(), 1);
        assert_eq!(damaged.len(), 1);
        assert_eq!(damaged[0].uuid, bad);
        assert!(store.snapshot_path(&bad).exists());
        let other = ProjectStore::open(root.path(), "p2").unwrap();
        assert!(other.tasks().unwrap().0.is_empty());
        assert!(store.find("T-zzzzzz").is_err());
        assert!(store.find("nonsense").is_err());
    }

    #[test]
    fn a_damaged_task_is_named_as_damaged() {
        let root = tempfile::tempdir().unwrap();
        let store = ProjectStore::open(root.path(), "p1").unwrap();
        let mut task = new_task(&store, "V");
        store.save(&mut task).unwrap();
        let bad = ids::new_uuid();
        fs::write(store.snapshot_path(&bad), b"not loro").unwrap();
        let by_uuid = store.find(&bad).err().unwrap();
        assert!(by_uuid.to_string().contains("is damaged"), "{by_uuid}");
        assert_eq!(by_uuid.kind(), ErrorKind::Failed);
        let by_id = store.find("T-zzzzzz").err().unwrap().to_string();
        assert!(by_id.contains("1 task file could not be read"), "{by_id}");
        let missing = store.find(&ids::new_uuid()).err().unwrap().to_string();
        assert!(missing.starts_with("no task"), "{missing}");
        let refused = task.set_stage(Stage::Done, &Actor::Person, 2).unwrap_err();
        assert_eq!(refused.kind(), ErrorKind::Refused);
    }

    #[test]
    fn leftover_temporary_files_are_swept_on_open() {
        let root = tempfile::tempdir().unwrap();
        let store = ProjectStore::open(root.path(), "p1").unwrap();
        let mut task = new_task(&store, "V");
        store.save(&mut task).unwrap();
        let strays = [
            store
                .dir
                .join("tasks")
                .join(format!("{}.tmp-{}", task.uuid(), ids::new_uuid())),
            store.dir.join(format!("project.tmp-{}", ids::new_uuid())),
        ];
        for stray in &strays {
            fs::write(stray, b"half written").unwrap();
        }
        drop(store);
        let store = ProjectStore::open(root.path(), "p1").unwrap();
        assert!(strays.iter().all(|s| !s.exists()));
        assert!(store.load(task.uuid()).is_ok());
    }

    #[test]
    fn a_task_that_cannot_be_shown_is_set_aside() {
        let root = tempfile::tempdir().unwrap();
        let store = ProjectStore::open(root.path(), "p1").unwrap();
        let mut good = new_task(&store, "V");
        store.save(&mut good).unwrap();
        let mut bad = new_task(&store, "W");
        bad.set_raw_field("target", "{\"kind\":\"team\"}");
        store.save(&mut bad).unwrap();
        let (tasks, damaged) = store.tasks().unwrap();
        assert_eq!(tasks.len(), 1);
        assert_eq!(damaged.len(), 1);
        assert_eq!(damaged[0].uuid, bad.uuid());
        // Listing a stage and counting agent starts still work.
        assert_eq!(store.ordered(Stage::Todo).unwrap().len(), 1);
        assert_eq!(store.agent_starts_since(0).unwrap(), 0);
    }

    #[test]
    fn ranks_place_tasks_even_between_equal_ranks() {
        let root = tempfile::tempdir().unwrap();
        let store = ProjectStore::open(root.path(), "p1").unwrap();
        assert_eq!(
            store.rank_for(Stage::Todo, &Position::End, None).unwrap(),
            "V"
        );
        for rank in ["V", "V", "X"] {
            let mut task = new_task(&store, rank);
            store.save(&mut task).unwrap();
        }
        let list = store.ordered(Stage::Todo).unwrap();
        let last = list[2].id();
        let between = store
            .rank_for(Stage::Todo, &Position::Index(1), None)
            .unwrap();
        assert!(
            "V" < between.as_str() && between.as_str() < "X",
            "{between}"
        );
        let before = store
            .rank_for(Stage::Todo, &Position::Before(last.clone()), None)
            .unwrap();
        assert!("V" < before.as_str() && before.as_str() < "X", "{before}");
        assert!(
            store
                .rank_for(Stage::Todo, &Position::Top, None)
                .unwrap()
                .as_str()
                < "V"
        );
        assert!(
            store
                .rank_for(Stage::Todo, &Position::End, None)
                .unwrap()
                .as_str()
                > "X"
        );
        let after_last = store
            .rank_for(
                Stage::Todo,
                &Position::After(last.clone()),
                Some(list[2].uuid()),
            )
            .is_err();
        assert!(after_last, "the moving task is not its own anchor");
        assert!(store
            .rank_for(Stage::Backlog, &Position::Before(last), None)
            .is_err());
    }

    #[test]
    fn a_listing_loads_every_task_once_in_rank_order() {
        let root = tempfile::tempdir().unwrap();
        let store = ProjectStore::open(root.path(), "p1").unwrap();
        let mut ids = Vec::new();
        for rank in ["X", "V", "W"] {
            let mut task = new_task(&store, rank);
            store.save(&mut task).unwrap();
            ids.push(task.id());
        }
        fs::write(store.snapshot_path(&ids::new_uuid()), b"not loro").unwrap();
        let (tasks, damaged) = store.listing().unwrap();
        let order: Vec<String> = tasks.iter().map(TaskDoc::id).collect();
        assert_eq!(order, [ids[1].clone(), ids[2].clone(), ids[0].clone()]);
        assert_eq!(damaged.len(), 1);
    }

    #[test]
    fn long_ranks_are_renumbered_in_order() {
        let root = tempfile::tempdir().unwrap();
        let store = ProjectStore::open(root.path(), "p1").unwrap();
        // Ranks that many moves to the top leave behind.
        let long = |last: char| format!("{}{last}", "0".repeat(40));
        for rank in [long('1'), long('2'), "V".to_owned()] {
            let mut task = new_task(&store, &rank);
            store.save(&mut task).unwrap();
        }
        let before: Vec<String> = store
            .ordered(Stage::Todo)
            .unwrap()
            .iter()
            .map(TaskDoc::id)
            .collect();
        let top = store.rank_for(Stage::Todo, &Position::Top, None).unwrap();
        let after = store.ordered(Stage::Todo).unwrap();
        assert_eq!(after.iter().map(TaskDoc::id).collect::<Vec<_>>(), before);
        assert!(after.iter().all(|t| t.rank().len() <= 5), "renumbered");
        assert!(top.len() <= 5 && top < after[0].rank(), "{top}");
    }

    #[test]
    fn agent_starts_are_counted() {
        let root = tempfile::tempdir().unwrap();
        let store = ProjectStore::open(root.path(), "p1").unwrap();
        let mut by_agent = new_task(&store, "V");
        by_agent
            .record_send(&api(), "r1", &Actor::Agent(api()), 1_000)
            .unwrap();
        store.save(&mut by_agent).unwrap();
        let mut by_person = new_task(&store, "W");
        by_person
            .record_send(&api(), "r2", &Actor::Person, 1_000)
            .unwrap();
        store.save(&mut by_person).unwrap();
        assert_eq!(store.agent_starts_since(500).unwrap(), 1);
        assert_eq!(store.agent_starts_since(2_000).unwrap(), 0);
    }

    #[test]
    fn the_project_lock_is_exclusive() {
        let root = tempfile::tempdir().unwrap();
        let store = ProjectStore::open(root.path(), "p1").unwrap();
        let other = File::open(store.dir.join(".lock")).unwrap();
        assert!(other.try_lock_exclusive().is_err());
        drop(store);
        assert!(other.try_lock_exclusive().is_ok());
    }
}
