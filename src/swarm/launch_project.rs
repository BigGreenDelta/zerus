//! Exact launch placement with a private, local write-ahead receipt journal.
//! The existing catalog schema stays compatible with installed native clients.
//! An unfinished intent is permanently uncertain and is never replayed.
use super::{
    catalog::{new_id, Key},
    store::{LockedStore, Store},
};
use crate::config::Config;
use anyhow::{ensure, Result};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::path::Path;

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Request {
    request_id: String,
    name: String,
    expected_run_id: String,
    expected_conversation_id: String,
    swarm_id: String,
    project_id: String,
    directory: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    project_folder_id: Option<String>,
    add_folder: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    worktree_folder_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    worktree_common_dir: Option<String>,
}
fn canonical_uuid(id: &str) -> bool {
    uuid::Uuid::parse_str(id).is_ok_and(|id_uuid| !id_uuid.is_nil() && id_uuid.to_string() == id)
}
fn identifier(id: &str) -> bool {
    !id.is_empty() && id.len() <= 128 && id.trim() == id && !id.chars().any(char::is_control)
}
impl Request {
    fn outcome(&self, status: &str, conversation: &str) -> Value {
        json!({"request_id":self.request_id,"status":status,"name":self.name,
            "run_id":self.expected_run_id,"conversation_id":conversation,
            "swarm_id":self.swarm_id,"project_id":self.project_id})
    }
    fn validate(&self) -> Result<()> {
        ensure!(canonical_uuid(&self.request_id), "invalid request UUID");
        ensure!(canonical_uuid(&self.swarm_id), "invalid swarm UUID");
        ensure!(
            !self.name.is_empty()
                && self.name.len() <= 2048
                && !self.name.chars().any(char::is_control),
            "invalid native session name"
        );
        ensure!(
            !self.expected_run_id.is_empty()
                && self.expected_run_id.len() <= 128
                && !self.expected_run_id.chars().any(char::is_control),
            "invalid expected run ID"
        );
        ensure!(
            self.expected_conversation_id.len() <= 2048
                && !self.expected_conversation_id.chars().any(char::is_control),
            "invalid expected conversation ID"
        );
        ensure!(identifier(&self.project_id), "invalid project ID");
        ensure!(
            self.project_folder_id.as_deref().is_none_or(identifier),
            "invalid project folder ID"
        );
        ensure!(
            self.directory.len() <= 8192
                && Path::new(&self.directory).is_absolute()
                && !self.directory.chars().any(char::is_control),
            "directory must be absolute"
        );
        ensure!(
            self.worktree_folder_id.is_some() == self.worktree_common_dir.is_some(),
            "worktree placement needs anchor and repository identity"
        );
        if let (Some(anchor), Some(common)) = (&self.worktree_folder_id, &self.worktree_common_dir)
        {
            ensure!(
                !self.add_folder
                    && self.project_folder_id.is_none()
                    && identifier(anchor)
                    && common.len() <= 4096
                    && Path::new(common).is_absolute()
                    && !common.chars().any(char::is_control),
                "invalid worktree project placement"
            );
        }
        Ok(())
    }
    fn check_binding(&self, record: &Value) -> Result<String> {
        ensure!(
            record["name"] == self.name
                && record["run_id"] == self.expected_run_id
                && record["launch_id"] == self.request_id,
            "Native launch identity changed; no replacement was selected"
        );
        let conversation = record["conversation_id"].as_str().unwrap_or_default();
        // A bootstrap receipt can precede the first conversation identity.
        // Placement changes metadata only: a later clear remains the exact
        // launched session identified by UUID, run and name. Once the caller
        // observed a conversation, that identity must still match exactly.
        ensure!(
            self.expected_conversation_id.is_empty()
                || conversation == self.expected_conversation_id,
            "Native launch conversation changed"
        );
        let native_directory = record["launch_dir"]
            .as_str()
            .filter(|s| !s.is_empty())
            .or_else(|| record["cwd"].as_str())
            .ok_or_else(|| anyhow::anyhow!("Native launch directory is unavailable"))?;
        ensure!(
            Path::new(native_directory).canonicalize()?
                == Path::new(&self.directory).canonicalize()?,
            "Native launch directory does not match the requested directory"
        );
        Ok(conversation.to_owned())
    }
}

pub(super) fn execute(config: &Config, value: Value) -> Value {
    let request: Request = match serde_json::from_value(value.clone()) {
        Ok(request) => request,
        Err(error) => {
            return json!({"request_id":value["request_id"].as_str().unwrap_or_default(),"status":"failed",
            "name":value["name"].as_str().unwrap_or_default(),"run_id":value["expected_run_id"].as_str().unwrap_or_default(),
            "conversation_id":value["expected_conversation_id"].as_str().unwrap_or_default(),
            "swarm_id":value["swarm_id"].as_str().unwrap_or_default(),"project_id":value["project_id"].as_str().unwrap_or_default(),"error":error.to_string()})
        }
    };
    let mut result = request.outcome("failed", &request.expected_conversation_id);
    let attempt = (|| -> Result<Value> {
        request.validate()?;
        // Read a durable receipt before checking the current launch: a duplicate
        // must preserve subsequent manual moves and can outlive the native run.
        let store = LockedStore::load(config)?;
        let journal_path = store.path.with_file_name("launch-assignments.json");
        let receipts = read_receipts(&journal_path)?;
        if let Some(outcome) = recorded(&receipts, &request)? {
            return Ok(outcome);
        }
        drop(store);
        crate::state::with_launch_binding(&request.name, |record| {
            let conversation = request.check_binding(record)?;
            let mut store = LockedStore::load(config)?;
            let mut receipts = read_receipts(&journal_path)?;
            if let Some(outcome) = recorded(&receipts, &request)? {
                return Ok(outcome);
            }
            check_capacity(&receipts)?;
            let assigned = assign(&mut store.data, &request, &conversation);
            let mut outcome = request.outcome(
                if assigned.is_ok() {
                    "assigned"
                } else {
                    "failed"
                },
                &conversation,
            );
            match &assigned {
                Ok(folder) => outcome["folder_id"] = json!(folder),
                Err(error) => outcome["error"] = json!(error.to_string()),
            }
            let mut uncertain = request.outcome("uncertain", &conversation);
            uncertain["error"] = json!("An assignment intent exists without a confirmed outcome; metadata will not be changed again");
            receipts.insert(
                request.request_id.clone(),
                json!({"request":request,"phase":"attempting","outcome":uncertain}),
            );
            // A failed intent write prevents any catalog mutation. If the rename
            // happened before fsync failed, a later retry conservatively sees
            // the unfinished intent and never repeats the assignment.
            write_receipts(&journal_path, &receipts)?;
            if assigned.is_ok() {
                if let Err(error) = store.save() {
                    uncertain["error"] = json!(format!(
                        "Assignment persistence could not be confirmed: {error}"
                    ));
                    return Ok(uncertain);
                }
            }
            receipts.insert(
                request.request_id.clone(),
                json!({"request":request,"phase":"terminal","outcome":outcome}),
            );
            if let Err(error) = write_receipts(&journal_path, &receipts) {
                uncertain["error"] = json!(format!(
                    "Assignment outcome persistence could not be confirmed: {error}"
                ));
                return Ok(uncertain);
            }
            Ok(outcome)
        })
    })();
    match attempt {
        Ok(outcome) => outcome,
        Err(error) => {
            result["error"] = json!(error.to_string());
            result
        }
    }
}

// Receipts never expire: eviction could permit an old request to undo a later
// manual move. At capacity, reject new requests before any assignment write.
pub(super) type Receipts = std::collections::BTreeMap<String, Value>;
pub(super) fn check_capacity(receipts: &Receipts) -> Result<()> {
    ensure!(
        receipts.len() < 10_000,
        "Launch assignment receipt capacity reached; no metadata was changed"
    );
    Ok(())
}
fn read_receipts(path: &Path) -> Result<Receipts> {
    use std::io::Read;
    let file = match std::fs::File::open(path) {
        Ok(file) => file,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(Receipts::new()),
        Err(error) => return Err(error.into()),
    };
    let mut bytes = Vec::new();
    file.take(128 * 1024 * 1024 + 1).read_to_end(&mut bytes)?;
    ensure!(
        bytes.len() <= 128 * 1024 * 1024,
        "Launch assignment journal is too large"
    );
    let receipts: Receipts = serde_json::from_slice(&bytes)?;
    ensure!(
        receipts.len() <= 10_000,
        "Launch assignment journal has too many receipts"
    );
    Ok(receipts)
}
pub(super) fn write_receipts(path: &Path, receipts: &Receipts) -> Result<()> {
    use std::io::Write;
    let parent = path
        .parent()
        .ok_or_else(|| anyhow::anyhow!("Invalid launch receipt path"))?;
    let mut temporary = tempfile::NamedTempFile::new_in(parent)?;
    crate::platform::make_private(temporary.as_file())?;
    temporary.write_all(&serde_json::to_vec(receipts)?)?;
    temporary.as_file().sync_all()?;
    crate::platform::persist(temporary, path)?;
    crate::platform::sync_dir(parent)?;
    Ok(())
}
fn recorded(receipts: &Receipts, request: &Request) -> Result<Option<Value>> {
    let Some(receipt) = receipts.get(&request.request_id) else {
        return Ok(None);
    };
    ensure!(
        receipt["request"] == serde_json::to_value(request)?,
        "Request UUID already has another assignment payload"
    );
    ensure!(
        receipt["phase"] == "attempting" || receipt["phase"] == "terminal",
        "Invalid launch assignment receipt phase"
    );
    let mut outcome = receipt["outcome"].clone();
    ensure!(
        outcome.is_object(),
        "Invalid launch assignment receipt outcome"
    );
    if receipt["phase"] == "attempting" {
        outcome["status"] = json!("uncertain");
    }
    Ok(Some(outcome))
}

fn assign(store: &mut Store, request: &Request, _conversation: &str) -> Result<String> {
    ensure!(store.imported, "initialize the local project catalog first");
    ensure!(
        store.catalog.swarm_id == request.swarm_id,
        "Project swarm identity changed"
    );
    let values = store.catalog.values();
    ensure!(
        values.get(&Key::project(&request.project_id, "alive")) == Some(&json!(true)),
        "Requested project no longer exists"
    );
    let directory = Path::new(&request.directory).canonicalize()?;
    ensure!(directory.is_dir(), "Launch directory is not a directory");
    let path = directory
        .to_str()
        .ok_or_else(|| anyhow::anyhow!("Launch directory is not UTF-8"))?;
    let local = store.catalog.machine(&store.node_id);
    let matches = |location: &Value| {
        location["project"] == request.project_id
            && location["machine"]
                .as_str()
                .is_some_and(|id| store.catalog.machine(id) == local)
            && location["path"].as_str().is_some_and(|saved| {
                Path::new(saved)
                    .canonicalize()
                    .is_ok_and(|saved| saved == directory)
            })
    };
    let existing = if let Some(anchor) = &request.worktree_folder_id {
        let folder = values
            .get(&Key::Folder { id: anchor.clone() })
            .ok_or_else(|| anyhow::anyhow!("Requested worktree project anchor no longer exists"))?;
        ensure!(
            folder["project"] == request.project_id
                && folder["machine"]
                    .as_str()
                    .is_some_and(|id| store.catalog.machine(id) == local),
            "Worktree anchor is outside this local project"
        );
        let anchor_path = Path::new(
            folder["path"]
                .as_str()
                .ok_or_else(|| anyhow::anyhow!("Invalid worktree anchor path"))?,
        )
        .canonicalize()?;
        let evidence = crate::state::worktree_catalog(&anchor_path);
        ensure!(
            evidence["state"] == "ok"
                && evidence["stale"] == false
                && evidence["common_dir"].as_str() == request.worktree_common_dir.as_deref(),
            "Worktree repository identity changed"
        );
        let contains = |path: &Path| {
            evidence["worktrees"].as_array().is_some_and(|trees| {
                trees.iter().any(|tree| {
                    tree["available"] == true
                        && tree["kind"] != "bare"
                        && tree["path"]
                            .as_str()
                            .is_some_and(|root| path == Path::new(root))
                })
            })
        };
        ensure!(
            contains(&directory) && contains(&anchor_path),
            "Launch directory is not a verified worktree of this project"
        );
        Some(anchor.clone())
    } else if let Some(id) = &request.project_folder_id {
        let folder = values
            .get(&Key::Folder { id: id.clone() })
            .ok_or_else(|| anyhow::anyhow!("Requested project folder no longer exists"))?;
        ensure!(
            matches(folder),
            "Requested folder does not match this local project and directory"
        );
        Some(id.clone())
    } else {
        values.iter().find_map(|(key, value)| match key {
            Key::Folder { id } if matches(value) => Some(id.clone()),
            _ => None,
        })
    };
    ensure!(
        existing.is_some() || request.add_folder,
        "Directory is outside the project; explicit folder addition is required"
    );
    // Stage on a clone so any validation/write failure commits no partial folder.
    let mut catalog = store.catalog.clone();
    let folder = existing.unwrap_or_else(new_id);
    if !values.contains_key(&Key::Folder { id: folder.clone() }) {
        catalog.set(
            &store.node_id,
            Key::Folder { id: folder.clone() },
            json!({"project":request.project_id,
            "machine":local,"path":path,"name":""}),
        )?;
    }
    catalog.set(
        &store.node_id,
        Key::Session {
            machine: local,
            session: request.name.clone(),
        },
        json!(request.project_id),
    )?;
    store.catalog = catalog;
    Ok(folder)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeMap;
    fn fixture() -> (tempfile::TempDir, Store, Request, Value) {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store {
            node_id: new_id(),
            catalog: Default::default(),
            imported: true,
            bindings: BTreeMap::new(),
            peers: BTreeMap::new(),
        };
        store
            .catalog
            .set(
                &store.node_id,
                Key::project("ungrouped", "alive"),
                json!(true),
            )
            .unwrap();
        let request = Request {
            request_id: new_id(),
            name: "codex/example/test".into(),
            expected_run_id: new_id(),
            expected_conversation_id: "".into(),
            swarm_id: store.catalog.swarm_id.clone(),
            project_id: "ungrouped".into(),
            directory: dir.path().to_str().unwrap().into(),
            project_folder_id: None,
            add_folder: true,
            worktree_folder_id: None,
            worktree_common_dir: None,
        };
        let record = json!({"name":request.name,"run_id":request.expected_run_id,"launch_id":request.request_id,
            "conversation_id":"new-conversation","launch_dir":request.directory});
        (dir, store, request, record)
    }

    // Shared folder paths are POSIX paths of a machine that runs sessions.
    #[cfg(unix)]
    #[test]
    fn durable_duplicate_returns_original_after_manual_move_even_at_capacity() {
        let (dir, mut store, request, _record) = fixture();
        let folder = assign(&mut store, &request, "").unwrap();
        let mut outcome = request.outcome("assigned", "original-conversation");
        outcome["folder_id"] = json!(folder);
        let mut receipts = Receipts::new();
        receipts.insert(
            request.request_id.clone(),
            json!({"request":request,"phase":"terminal","outcome":outcome}),
        );
        for n in 1..10_000 {
            receipts.insert(format!("reserved-{n}"), json!({}));
        }
        let key = Key::Session {
            machine: store.node_id.clone(),
            session: request.name.clone(),
        };
        store
            .catalog
            .set(&store.node_id, key.clone(), json!("later-choice"))
            .unwrap();
        assert!(check_capacity(&receipts).is_err());
        let config = Config {
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
        std::fs::create_dir_all(config.dir.join("swarm")).unwrap();
        std::fs::write(
            config.dir.join("swarm/catalog.json"),
            serde_json::to_vec(&store).unwrap(),
        )
        .unwrap();
        write_receipts(&config.dir.join("swarm/launch-assignments.json"), &receipts).unwrap();
        let before = std::fs::read(config.dir.join("swarm/catalog.json")).unwrap();
        assert_eq!(
            execute(&config, serde_json::to_value(&request).unwrap()),
            outcome
        );
        assert_eq!(
            std::fs::read(config.dir.join("swarm/catalog.json")).unwrap(),
            before
        );
        let mut reused = request.clone();
        reused.project_id = "another-project".into();
        assert_eq!(
            execute(&config, serde_json::to_value(&reused).unwrap())["status"],
            "failed"
        );
        assert_eq!(
            std::fs::read(config.dir.join("swarm/catalog.json")).unwrap(),
            before
        );
        assert_eq!(store.catalog.values()[&key], "later-choice");
    }
    #[test]
    fn unfinished_intent_never_reapplies_and_catalog_keeps_previous_schema() {
        let (dir, mut store, request, _record) = fixture();
        let key = Key::Session {
            machine: store.node_id.clone(),
            session: request.name.clone(),
        };
        store
            .catalog
            .set(&store.node_id, key, json!("later-choice"))
            .unwrap();
        let config = Config {
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
        std::fs::create_dir_all(config.dir.join("swarm")).unwrap();
        let bytes = serde_json::to_vec(&store).unwrap();
        // The previous deny_unknown_fields schema has exactly these fields.
        let catalog: Value = serde_json::from_slice(&bytes).unwrap();
        let keys: std::collections::BTreeSet<_> = catalog
            .as_object()
            .unwrap()
            .keys()
            .map(String::as_str)
            .collect();
        assert_eq!(
            keys,
            std::collections::BTreeSet::from([
                "node_id", "catalog", "imported", "bindings", "peers"
            ])
        );
        assert!(serde_json::from_slice::<Store>(&bytes).is_ok());
        std::fs::write(config.dir.join("swarm/catalog.json"), &bytes).unwrap();
        let receipts = Receipts::from([(
            request.request_id.clone(),
            json!({"request":request,
            "phase":"attempting","outcome":request.outcome("uncertain","observed-conversation")}),
        )]);
        write_receipts(&config.dir.join("swarm/launch-assignments.json"), &receipts).unwrap();
        for _ in 0..2 {
            let result = execute(&config, serde_json::to_value(&request).unwrap());
            assert_eq!(result["status"], "uncertain");
            assert_eq!(
                std::fs::read(config.dir.join("swarm/catalog.json")).unwrap(),
                bytes
            );
        }
        assert_eq!(
            read_receipts(&config.dir.join("swarm/launch-assignments.json")).unwrap(),
            receipts
        );
    }

    #[cfg(unix)]
    #[test]
    fn saved_symlink_folder_is_reused_without_rewriting_its_path() {
        let (dir, mut store, mut request, _record) = fixture();
        let alias_root = tempfile::tempdir().unwrap();
        let alias = alias_root.path().join("project-alias");
        std::os::unix::fs::symlink(dir.path(), &alias).unwrap();
        let original = json!({"project":"ungrouped","machine":store.node_id,
            "path":alias.to_str().unwrap(),"name":"Saved alias"});
        let key = Key::Folder {
            id: "saved-alias".into(),
        };
        store
            .catalog
            .set(&store.node_id, key.clone(), original.clone())
            .unwrap();
        request.project_folder_id = Some("saved-alias".into());
        request.add_folder = false;
        assert_eq!(assign(&mut store, &request, "").unwrap(), "saved-alias");
        assert_eq!(store.catalog.values()[&key], original);
        request.project_folder_id = None;
        assert_eq!(assign(&mut store, &request, "").unwrap(), "saved-alias");
        let other = tempfile::tempdir().unwrap();
        std::fs::remove_file(&alias).unwrap();
        std::os::unix::fs::symlink(other.path(), &alias).unwrap();
        request.project_folder_id = Some("saved-alias".into());
        assert!(assign(&mut store, &request, "").is_err());
    }

    // Shared folder paths are POSIX paths of a machine that runs sessions.
    #[cfg(unix)]
    #[test]
    fn related_worktree_keeps_anchor_and_rejects_nested_repository() {
        let (dir, mut store, mut request, _) = fixture();
        let repo = dir.path().join("repo");
        let linked = dir.path().join("linked");
        let git = |args: &[&str]| {
            assert!(std::process::Command::new("git")
                .args(args)
                .env("GIT_CONFIG_NOSYSTEM", "1")
                .env("GIT_CONFIG_GLOBAL", "/dev/null")
                .output()
                .unwrap()
                .status
                .success());
        };
        git(&["init", "-q", "-b", "main", repo.to_str().unwrap()]);
        git(&[
            "-C",
            repo.to_str().unwrap(),
            "-c",
            "user.name=Fixture",
            "-c",
            "user.email=fixture@example.test",
            "commit",
            "--allow-empty",
            "-qm",
            "initial",
        ]);
        git(&[
            "-C",
            repo.to_str().unwrap(),
            "worktree",
            "add",
            "-qb",
            "feature",
            linked.to_str().unwrap(),
        ]);
        let anchor = Key::Folder {
            id: "anchor".into(),
        };
        store
            .catalog
            .set(
                &store.node_id,
                anchor.clone(),
                json!({"project":"ungrouped","machine":store.node_id,"path":repo,"name":"Repo"}),
            )
            .unwrap();
        request.add_folder = false;
        request.worktree_folder_id = Some("anchor".into());
        request.worktree_common_dir = Some(
            repo.join(".git")
                .canonicalize()
                .unwrap()
                .to_str()
                .unwrap()
                .into(),
        );
        request.directory = linked.to_str().unwrap().into();
        assert_eq!(assign(&mut store, &request, "").unwrap(), "anchor");
        assert_eq!(
            store
                .catalog
                .values()
                .keys()
                .filter(|key| matches!(key, Key::Folder { .. }))
                .count(),
            1
        );
        let nested = repo.join("unrelated");
        git(&["init", "-q", nested.to_str().unwrap()]);
        request.directory = nested.to_str().unwrap().into();
        assert!(assign(&mut store, &request, "").is_err());
        request.directory = linked.to_str().unwrap().into();
        request.worktree_common_dir = Some(nested.join(".git").to_str().unwrap().into());
        assert!(assign(&mut store, &request, "").is_err());
    }

    #[test]
    fn bootstrap_conversation_exception_still_requires_exact_launch_run() {
        let (_dir, _store, mut request, mut record) = fixture();
        record["conversation_id"] = json!("conversation-after-clear");
        assert_eq!(
            request.check_binding(&record).unwrap(),
            "conversation-after-clear"
        );
        request.expected_conversation_id = "original-conversation".into();
        assert!(request.check_binding(&record).is_err());
        request.expected_conversation_id.clear();
        record["run_id"] = json!(new_id());
        assert!(request.check_binding(&record).is_err());
    }

    // Shared folder paths are POSIX paths of a machine that runs sessions.
    #[cfg(unix)]
    #[test]
    fn exact_binding_and_explicit_folder_addition() {
        let (_dir, mut store, request, record) = fixture();
        request.validate().unwrap();
        let conversation = request.check_binding(&record).unwrap();
        assert_eq!(conversation, "new-conversation");
        let folder = assign(&mut store, &request, &conversation).unwrap();
        let values = store.catalog.values();
        assert_eq!(
            values[&Key::Session {
                machine: store.node_id.clone(),
                session: request.name.clone()
            }],
            "ungrouped"
        );
        let expected_directory = Path::new(&request.directory).canonicalize().unwrap();
        assert_eq!(
            values[&Key::Folder { id: folder }]["path"],
            expected_directory.to_str().unwrap()
        );
    }
    #[test]
    fn wrong_launch_run_name_and_conversation_rejected() {
        let (_dir, _store, mut request, record) = fixture();
        for field in ["launch_id", "run_id", "name", "launch_dir"] {
            let mut changed = record.clone();
            changed[field] = json!("wrong");
            assert!(request.check_binding(&changed).is_err(), "{field}");
        }
        request.expected_conversation_id = "another-conversation".into();
        assert!(request.check_binding(&record).is_err());
        request.request_id = "not-a-uuid".into();
        assert!(request.validate().is_err());
    }
    #[test]
    fn bad_swarm_project_and_folder_do_not_write() {
        let (_dir, mut store, request, _record) = fixture();
        let before = serde_json::to_value(&store).unwrap();
        let mut wrong = request.clone();
        wrong.swarm_id = new_id();
        assert!(assign(&mut store, &wrong, "").is_err());
        wrong = request.clone();
        wrong.project_id = "missing".into();
        assert!(assign(&mut store, &wrong, "").is_err());
        wrong = request.clone();
        wrong.project_folder_id = Some("missing".into());
        assert!(assign(&mut store, &wrong, "").is_err());
        wrong = request.clone();
        wrong.add_folder = false;
        assert!(assign(&mut store, &wrong, "").is_err());
        assert_eq!(serde_json::to_value(&store).unwrap(), before);
    }
    // Shared folder paths are POSIX paths of a machine that runs sessions.
    #[cfg(unix)]
    #[test]
    fn selected_folder_must_belong_to_exact_local_project_path() {
        let (_dir, mut store, mut request, _record) = fixture();
        request.project_folder_id = Some("folder".into());
        for location in [
            json!({"project":"ungrouped","machine":new_id(),"path":request.directory,"name":""}),
            json!({"project":"other","machine":store.node_id,"path":request.directory,"name":""}),
            json!({"project":"ungrouped","machine":store.node_id,"path":"/reserved/example","name":""}),
        ] {
            store
                .catalog
                .set(
                    &store.node_id,
                    Key::Folder {
                        id: "folder".into(),
                    },
                    location,
                )
                .unwrap();
            assert!(assign(&mut store, &request, "").is_err());
        }
        store
            .catalog
            .set(
                &store.node_id,
                Key::Folder {
                    id: "folder".into(),
                },
                json!({"project":"ungrouped",
            "machine":store.node_id,"path":request.directory,"name":""}),
            )
            .unwrap();
        request.add_folder = false;
        assert_eq!(assign(&mut store, &request, "").unwrap(), "folder");
    }
}
