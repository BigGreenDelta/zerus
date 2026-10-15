//! Shared helpers for the vector and process tests: the golden vectors,
//! scratch trees below Cargo's target directory and Python's file tree view.
#![allow(dead_code)]
use rustix::fs::Mode;
use serde_json::{json, Map, Value};
use std::collections::BTreeMap;
use std::os::unix::fs::{MetadataExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Output, Stdio};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Mutex, MutexGuard, PoisonError};
use zerus_mobile_connector::journal::Journal;

/// Every clock in the vectors is relative to this instant.
pub const EPOCH: f64 = 1_760_000_000.0;

pub fn vectors() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../vectors")
}

/// A vector file, read with the relay codec (the files hold the serde magic key).
pub fn document(name: &str) -> Value {
    let raw = std::fs::read(vectors().join(name)).unwrap();
    zerus_relay::json::parse(&raw, 64, 1_000_000).unwrap()
}

pub fn records(name: &str) -> Vec<Value> {
    std::fs::read_to_string(vectors().join(name))
        .unwrap()
        .lines()
        .map(|line| zerus_relay::json::parse(line.as_bytes(), 64, 1_000_000).unwrap())
        .collect()
}

/// A child that another test thread forks holds a copy of every descriptor,
/// a journal's lock included, until it executes its program. Spawning, and
/// closing a journal, only under this lock makes a closed journal free at once
/// for the next process.
static SPAWN: Mutex<()> = Mutex::new(());

fn spawn_gate() -> MutexGuard<'static, ()> {
    SPAWN.lock().unwrap_or_else(PoisonError::into_inner)
}

/// `command.spawn()`, which returns once the child runs its own program.
pub fn spawn(command: &mut Command) -> Child {
    let _gate = spawn_gate();
    command.spawn().unwrap()
}

/// `command.output()` with stdin closed, spawned under the gate.
pub fn captured(command: &mut Command) -> Output {
    spawn(
        command
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped()),
    )
    .wait_with_output()
    .unwrap()
}

/// Closes a journal while no child is being spawned, releasing its lock.
pub fn close(journal: Journal) {
    let _gate = spawn_gate();
    journal.close();
}

/// The vectors were captured with umask 022. Every test that compares modes
/// sets the same mask, so concurrent tests never disagree.
pub fn umask() {
    rustix::process::umask(Mode::from_raw_mode(0o022));
}

/// A private scratch directory below Cargo's target directory, removed on drop.
pub struct Scratch {
    pub root: PathBuf,
}

impl Scratch {
    pub fn new(label: &str) -> Self {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        let name = format!(
            "{}-{}-{}",
            label.replace('/', "-"),
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        );
        let root = Path::new(env!("CARGO_TARGET_TMPDIR")).join(name);
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        std::fs::set_permissions(&root, std::fs::Permissions::from_mode(0o700)).unwrap();
        // canonicalize: the vectors' paths are resolved ones.
        Scratch {
            root: root.canonicalize().unwrap(),
        }
    }
    pub fn path(&self, relative: &str) -> PathBuf {
        self.root.join(relative)
    }
    /// `$SCRATCH`, `$HOME` and `$CWD` in a vector string.
    pub fn substitute(&self, text: &str) -> String {
        text.replace("$SCRATCH", &self.root.display().to_string())
    }
    /// The inverse, for comparing effective paths with a vector.
    pub fn portable(&self, path: &Path) -> String {
        let text = path.display().to_string();
        let root = self.root.display().to_string();
        text.replace(&format!("{root}/home"), "$HOME")
            .replace(&format!("{root}/cwd"), "$CWD")
            .replace(&root, "$SCRATCH")
    }
}

impl Drop for Scratch {
    fn drop(&mut self) {
        // Directories a test made unreadable must not block the cleanup.
        let _ = std::fs::remove_dir_all(&self.root);
    }
}

/// capture.py `setup_paths`.
pub fn setup(root: &Path, actions: &Value) {
    for action in actions.as_array().unwrap() {
        let path = root.join(action["path"].as_str().unwrap());
        let mode = || u32::from_str_radix(action["mode"].as_str().unwrap(), 8).unwrap();
        match action["action"].as_str().unwrap() {
            "mkdir" => {
                std::fs::create_dir(&path).unwrap();
                std::fs::set_permissions(&path, std::fs::Permissions::from_mode(mode())).unwrap();
            }
            "chmod" => {
                std::fs::set_permissions(&path, std::fs::Permissions::from_mode(mode())).unwrap()
            }
            "write" => {
                std::fs::write(&path, action["text"].as_str().unwrap_or("")).unwrap();
                std::fs::set_permissions(&path, std::fs::Permissions::from_mode(mode())).unwrap();
            }
            "symlink" => {
                std::os::unix::fs::symlink(action["target"].as_str().unwrap(), &path).unwrap()
            }
            "hardlink" => {
                std::fs::hard_link(root.join(action["target"].as_str().unwrap()), &path).unwrap()
            }
            other => panic!("unknown setup action {other}"),
        }
    }
}

/// capture.py `tree`: every path below root with its kind and mode, skipping
/// the top-level `home` and `cwd` directories themselves.
pub fn tree(root: &Path) -> Value {
    fn walk(root: &Path, directory: &Path, found: &mut BTreeMap<String, Value>) {
        let mut entries: Vec<_> = std::fs::read_dir(directory)
            .unwrap()
            .map(|entry| entry.unwrap().path())
            .collect();
        entries.sort();
        for path in entries {
            let relative = path.strip_prefix(root).unwrap().display().to_string();
            let metadata = path.symlink_metadata().unwrap();
            let top = !relative.contains('/');
            if metadata.file_type().is_symlink() {
                found.insert(relative, json!({"kind": "symlink"}));
                continue;
            }
            let kind = if metadata.is_dir() { "dir" } else { "file" };
            if !(top && (relative == "home" || relative == "cwd")) {
                found.insert(
                    relative.clone(),
                    json!({"kind": kind, "mode": format!("{:04o}", metadata.mode() & 0o7777)}),
                );
            }
            if metadata.is_dir() {
                walk(root, &path, found);
            }
        }
    }
    let mut found = BTreeMap::new();
    walk(root, root, &mut found);
    Value::Object(found.into_iter().collect())
}

/// JSON equality where numbers compare by value: SQLite REAL 1760000000.0 and
/// a vector's 1760000000.0 differ in their serde representation.
pub fn same(a: &Value, b: &Value) -> bool {
    match (a, b) {
        (Value::Number(x), Value::Number(y)) => x.as_f64() == y.as_f64(),
        (Value::Array(x), Value::Array(y)) => {
            x.len() == y.len() && x.iter().zip(y).all(|(x, y)| same(x, y))
        }
        (Value::Object(x), Value::Object(y)) => {
            x.len() == y.len()
                && x.iter()
                    .all(|(key, value)| y.get(key).is_some_and(|other| same(value, other)))
        }
        _ => a == b,
    }
}

#[track_caller]
pub fn assert_same(actual: &Value, expected: &Value, context: &str) {
    assert!(
        same(actual, expected),
        "{context}\n actual: {actual}\nexpected: {expected}"
    );
}

pub fn sql_value(value: rusqlite::types::ValueRef<'_>) -> Value {
    use rusqlite::types::ValueRef;
    match value {
        ValueRef::Null => Value::Null,
        ValueRef::Integer(number) => json!(number),
        ValueRef::Real(number) => json!(number),
        ValueRef::Text(text) => json!(std::str::from_utf8(text).unwrap()),
        ValueRef::Blob(_) => panic!("journal values are never blobs"),
    }
}

pub fn read_only(path: &Path) -> rusqlite::Connection {
    let flags =
        rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY | rusqlite::OpenFlags::SQLITE_OPEN_NO_MUTEX;
    rusqlite::Connection::open_with_flags(path, flags).unwrap()
}

/// Rows of one query as JSON lists.
pub fn query(db: &rusqlite::Connection, sql: &str) -> Vec<Value> {
    let mut statement = db.prepare(sql).unwrap();
    let width = statement.column_count();
    statement
        .query_map([], |row| {
            Ok(Value::Array(
                (0..width)
                    .map(|index| sql_value(row.get_ref(index).unwrap()))
                    .collect(),
            ))
        })
        .unwrap()
        .map(Result::unwrap)
        .collect()
}

/// capture.py `schema` without the connection pragmas.
pub fn schema(path: &Path) -> Map<String, Value> {
    let db = read_only(path);
    let master: Vec<Value> = query(
        &db,
        "SELECT type,name,tbl_name,sql FROM sqlite_master ORDER BY rowid",
    )
    .into_iter()
    .map(|row| json!({"type": row[0], "name": row[1], "tbl_name": row[2], "sql": row[3]}))
    .collect();
    let names = |kind: &str| -> Vec<String> {
        master
            .iter()
            .filter(|row| row["type"] == kind)
            .map(|row| row["name"].as_str().unwrap().to_owned())
            .collect()
    };
    let per = |names: Vec<String>, pragma: &str| -> Value {
        Value::Object(
            names
                .into_iter()
                .map(|name| {
                    let rows = query(&db, &format!("PRAGMA {pragma}(\"{name}\")"));
                    (name, Value::Array(rows))
                })
                .collect(),
        )
    };
    let mut result = Map::new();
    result.insert("table_info".into(), per(names("table"), "table_info"));
    result.insert("index_list".into(), per(names("table"), "index_list"));
    result.insert("index_xinfo".into(), per(names("index"), "index_xinfo"));
    let mut pragmas = Map::new();
    for name in [
        "user_version",
        "application_id",
        "journal_mode",
        "auto_vacuum",
        "encoding",
        "page_size",
    ] {
        pragmas.insert(
            name.into(),
            query(&db, &format!("PRAGMA {name}"))[0][0].clone(),
        );
    }
    result.insert("pragmas".into(), Value::Object(pragmas));
    result.insert("sqlite_master".into(), Value::Array(master));
    result
}

/// `SELECT * FROM requests ORDER BY request_id` as objects.
pub fn rows(path: &Path) -> Vec<Value> {
    let db = read_only(path);
    let mut statement = db
        .prepare("SELECT * FROM requests ORDER BY request_id")
        .unwrap();
    let names: Vec<String> = statement
        .column_names()
        .into_iter()
        .map(str::to_owned)
        .collect();
    statement
        .query_map([], |row| {
            Ok(Value::Object(
                names
                    .iter()
                    .enumerate()
                    .map(|(index, name)| (name.clone(), sql_value(row.get_ref(index).unwrap())))
                    .collect(),
            ))
        })
        .unwrap()
        .map(Result::unwrap)
        .collect()
}

/// Stored receipt text is compared by meaning and by length: key order inside
/// native results may differ from Python's insertion order.
pub fn same_rows(actual: &[Value], expected: &Value) -> bool {
    let expected = expected.as_array().unwrap();
    actual.len() == expected.len()
        && actual.iter().zip(expected).all(|(actual, expected)| {
            let (Some(a), Some(b)) = (actual.as_object(), expected.as_object()) else {
                return false;
            };
            a.len() == b.len()
                && a.iter().all(|(key, value)| {
                    let other = &b[key];
                    match (key.as_str(), value.as_str(), other.as_str()) {
                        ("response", Some(x), Some(y)) => {
                            x.len() == y.len()
                                && same(
                                    &serde_json::from_str(x).unwrap(),
                                    &serde_json::from_str(y).unwrap(),
                                )
                        }
                        _ => same(value, other),
                    }
                })
        })
}

/// A journal fixture becomes journal.sqlite3, pre-created 0600 in a 0700 directory.
pub fn load_fixture(directory: &Path, sql: &str) {
    if !directory.exists() {
        std::fs::create_dir(directory).unwrap();
        std::fs::set_permissions(directory, std::fs::Permissions::from_mode(0o700)).unwrap();
    }
    let path = directory.join("journal.sqlite3");
    std::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&path)
        .unwrap();
    std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600)).unwrap();
    let db = rusqlite::Connection::open(&path).unwrap();
    db.execute_batch(sql).unwrap();
}
