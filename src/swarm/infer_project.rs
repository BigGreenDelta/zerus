//! Optional initial grouping. Never initialize a catalog, replace any explicit
//! membership (including a tombstone), infer by a path prefix, or create Git data.
use super::{
    catalog::Key,
    store::{LockedStore, Store},
};
use crate::config::Config;
use serde_json::{json, Value};
use std::{
    collections::BTreeSet,
    path::Path,
    time::{Duration, Instant},
};

/// Called only after a genuinely new native binding was created, never resume,
/// fork, listing, GUI inspection or adoption. Failures leave normal launch alone.
pub(crate) fn infer_new_session(config: &Config, name: &str, directory: &Path) {
    if !config.dir.join("swarm/catalog.json").is_file() {
        return;
    }
    let end = Instant::now() + Duration::from_secs(3);
    let attempt = (|| -> anyhow::Result<()> {
        let mut store = LockedStore::try_load_scoped(config)?;
        let project = infer(&store.data, name, directory, end, |directory| {
            crate::state::worktree_catalog_until(directory, end)
        });
        if let Some(project) = project {
            if Instant::now() >= end {
                return Ok(());
            }
            let key = Key::Session {
                machine: store.data.catalog.machine(&store.data.node_id),
                session: name.into(),
            };
            let actor = store.data.node_id.clone();
            store.data.catalog.set(&actor, key, json!(project))?;
            store.save_catalog_only()?;
        }
        Ok(())
    })();
    // Contention, inaccessible folders and optional metadata failures never
    // become an agent start failure, an automatic retry, or a default assignment.
    let _ = attempt;
}

/// An additive hint for `dirs`: optional read of an existing catalog, never
/// enrollment, initialization or membership mutation. Older peers omit it.
pub(crate) fn directory_project(config: &Config, directory: &Path) -> Option<Value> {
    if !config.dir.join("swarm/catalog.json").is_file() {
        return None;
    }
    let end = Instant::now() + Duration::from_secs(3);
    let store = LockedStore::try_load_scoped(config).ok()?;
    let project = infer_directory(&store.data, directory, end, |path| {
        crate::state::cached_worktree_catalog(path).unwrap_or(Value::Null)
    })?;
    Some(json!({"schema":1,"swarm_id":store.data.catalog.swarm_id,"project_id":project}))
}
fn infer(
    store: &Store,
    name: &str,
    directory: &Path,
    end: Instant,
    catalog: impl FnOnce(&Path) -> Value,
) -> Option<String> {
    let key = Key::Session {
        machine: store.catalog.machine(&store.node_id),
        session: name.into(),
    };
    if store.catalog.heads().contains_key(&key) {
        return None;
    }
    infer_directory(store, directory, end, catalog)
}
fn infer_directory(
    store: &Store,
    directory: &Path,
    end: Instant,
    catalog: impl FnOnce(&Path) -> Value,
) -> Option<String> {
    if !store.imported || Instant::now() >= end {
        return None;
    }
    let local = store.catalog.machine(&store.node_id);
    let directory = crate::platform::canonical(directory)
        .ok()
        .filter(|path| path.is_dir())?;
    let values = store.catalog.values();
    let mut anchors = Vec::new();
    for (key, value) in &values {
        if !matches!(key, Key::Folder { .. })
            || value.is_null()
            || value["machine"]
                .as_str()
                .is_none_or(|id| store.catalog.machine(id) != local)
        {
            continue;
        }
        let project = value["project"].as_str()?;
        if project == "ungrouped"
            || values.get(&Key::project(project, "alive")) != Some(&json!(true))
        {
            continue;
        }
        // A bounded complete candidate set; never decide from a truncated scan.
        if anchors.len() >= 512 || Instant::now() >= end {
            return None;
        }
        let path = crate::platform::canonical(value["path"].as_str()?)
            .ok()
            .filter(|path| path.is_dir())?;
        anchors.push((project.to_owned(), path));
    }
    if anchors.is_empty() || Instant::now() >= end {
        return None;
    }
    let mut projects: BTreeSet<String> = anchors
        .iter()
        .filter(|(_, path)| *path == directory)
        .map(|(id, _)| id.clone())
        .collect();
    let evidence = catalog(&directory);
    if Instant::now() >= end {
        return None;
    }
    if evidence["state"] == "ok"
        && evidence["stale"] == false
        && evidence["partial"] == false
        && evidence["path"].as_str() == directory.to_str()
        && evidence["common_dir"]
            .as_str()
            .is_some_and(|path| Path::new(path).is_absolute())
    {
        let entries = evidence["worktrees"].as_array()?;
        let contains = |path: &Path| {
            entries.iter().any(|tree| {
                tree["available"] == true
                    && matches!(tree["kind"].as_str(), Some("main" | "linked"))
                    && tree["path"].as_str() == path.to_str()
            })
        };
        if contains(&directory) {
            for (id, path) in anchors {
                if contains(&path) {
                    projects.insert(id);
                }
            }
        }
    }
    // Exact canonical folder membership stands independently of Git. Missing
    // or stale repository evidence never establishes a related worktree.
    if projects.len() == 1 {
        projects.pop_first()
    } else {
        None
    }
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    use crate::swarm::catalog::{new_id, Catalog};
    use std::collections::BTreeMap;
    fn store() -> Store {
        Store {
            node_id: new_id(),
            catalog: Catalog::default(),
            imported: true,
            bindings: BTreeMap::new(),
            peers: BTreeMap::new(),
        }
    }
    fn folder(store: &mut Store, id: &str, project: &str, machine: &str, path: &Path) {
        store
            .catalog
            .set(&store.node_id, Key::project(project, "alive"), json!(true))
            .unwrap();
        store
            .catalog
            .set(
                &store.node_id,
                Key::Folder { id: id.into() },
                json!({"project":project,"machine":machine,"path":path,"name":""}),
            )
            .unwrap();
    }
    fn choose(store: &Store, path: &Path, evidence: Value) -> Option<String> {
        infer(
            store,
            "codex/example/new",
            path,
            Instant::now() + Duration::from_secs(3),
            |_| evidence,
        )
    }
    #[cfg(unix)]
    #[test]
    fn canonical_exact_non_git_folder_is_initial_metadata_only() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().canonicalize().unwrap();
        let mut store = store();
        let local = store.node_id.clone();
        folder(&mut store, "folder", "work", &local, &path);
        assert_eq!(
            choose(&store, &path, json!({"state":"not_repo"})),
            Some("work".into())
        );
        assert!(!path.join(".git").exists());
        let link = path.join("alias");
        std::os::unix::fs::symlink(&path, &link).unwrap();
        assert_eq!(
            choose(&store, &link, json!({"state":"not_repo"})),
            Some("work".into())
        );
        let nested = path.join("nested");
        std::fs::create_dir(&nested).unwrap();
        assert_eq!(choose(&store, &nested, json!({"state":"not_repo"})), None); // no prefix guess
        for value in [Value::Null, json!("ungrouped"), json!("manual")] {
            let mut explicit = store.clone();
            explicit
                .catalog
                .set(
                    &local,
                    Key::Session {
                        machine: local.clone(),
                        session: "codex/example/new".into(),
                    },
                    value,
                )
                .unwrap();
            assert_eq!(choose(&explicit, &path, json!({"state":"not_repo"})), None);
        }
    }
    #[cfg(unix)]
    #[test]
    fn exact_catalog_worktrees_require_complete_same_machine_unambiguous_evidence() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().canonicalize().unwrap();
        let anchor = root.join("main");
        let linked = root.join("linked");
        std::fs::create_dir(&anchor).unwrap();
        std::fs::create_dir(&linked).unwrap();
        let evidence = json!({"state":"ok","stale":false,"partial":false,"path":linked,"common_dir":root.join("common"),
            "worktrees":[{"path":anchor,"kind":"main","available":true},{"path":linked,"kind":"linked","available":true}]});
        let mut store = store();
        let local = store.node_id.clone();
        folder(&mut store, "anchor", "work", &local, &anchor);
        assert_eq!(
            choose(&store, &linked, evidence.clone()),
            Some("work".into())
        );
        for field in ["stale", "partial"] {
            let mut changed = evidence.clone();
            changed[field] = json!(true);
            assert_eq!(choose(&store, &linked, changed), None);
        }
        for changed in [
            json!({"state":"timeout"}),
            json!({"state":"ok","stale":false,"partial":false,"path":linked,"common_dir":"relative","worktrees":[]}),
        ] {
            assert_eq!(choose(&store, &linked, changed), None);
        }
        for field in ["available", "kind"] {
            let mut changed = evidence.clone();
            changed["worktrees"][0][field] = if field == "available" {
                json!(false)
            } else {
                json!("bare")
            };
            assert_eq!(choose(&store, &linked, changed), None);
        }
        let mut remote = store.clone();
        let other = new_id();
        folder(&mut remote, "anchor", "work", &other, &anchor);
        assert_eq!(choose(&remote, &linked, evidence.clone()), None);
        folder(&mut store, "second", "other", &local, &linked);
        assert_eq!(choose(&store, &linked, evidence), None);
    }
    #[cfg(unix)]
    #[test]
    fn expired_or_uninitialized_inference_does_not_consult_git() {
        let dir = tempfile::tempdir().unwrap();
        let mut store = store();
        let never = |_: &Path| -> Value {
            panic!("Git must not run");
        };
        assert_eq!(
            infer(&store, "one", dir.path(), Instant::now(), never),
            None
        );
        store.imported = false;
        assert_eq!(
            infer(
                &store,
                "one",
                dir.path(),
                Instant::now() + Duration::from_secs(3),
                never
            ),
            None
        );
    }
}
