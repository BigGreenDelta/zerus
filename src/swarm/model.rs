use super::{
    catalog::{Catalog, Key},
    store::Store,
};
use crate::config::Config;
use anyhow::{ensure, Result};
use serde::Deserialize;
use serde_json::{json, Value};
use std::collections::{BTreeMap, BTreeSet};

/// A drive (`C:\`, `c:/`) or UNC (`\\server\share`, `//server/share`) path: on this
/// machine, a local Windows folder of a desktop (D15 of the Windows native host design).
/// The catalog has no Windows path kind yet, so these stay in that desktop's
/// organization. A peer's `//server/share` is a POSIX path, which a Windows GUI can
/// store (`QDir::cleanPath` keeps the leading `//` there). Matches
/// `MachinePlatform::windowsShaped` in the GUI.
fn windows_folder(path: &str) -> bool {
    let separator = |c: &u8| *c == b'/' || *c == b'\\';
    match path.as_bytes() {
        [drive, b':', next, ..] if drive.is_ascii_alphabetic() && separator(next) => true,
        // Two separators, a server name, a separator and a share name.
        [first, second, rest @ ..] if separator(first) && separator(second) => {
            let end = rest.iter().position(separator);
            end.is_some_and(|end| end > 0 && rest.get(end + 1).is_some_and(|c| !separator(c)))
        }
        _ => false,
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Patch {
    pub base: Value,
    pub desired: Value,
    pub versions: BTreeMap<String, Vec<String>>,
}

impl Store {
    pub fn flatten(&mut self, organization: &Value) -> Result<BTreeMap<Key, Value>> {
        let mut fields = BTreeMap::new();
        let local = self.catalog.machine(&self.node_id);
        let mut machines = BTreeMap::<String, String>::new();
        let mut machine_id = |alias: &str| -> Result<String> {
            if let Some(id) = machines.get(alias) {
                return Ok(id.clone());
            }
            let id = self.machine_id(alias)?;
            machines.insert(alias.into(), id.clone());
            Ok(id)
        };
        let projects = organization["projects"]
            .as_array()
            .ok_or_else(|| anyhow::anyhow!("organization needs projects"))?;
        ensure!(projects.len() <= 5000, "too many projects");
        for p in projects {
            let id = p["id"]
                .as_str()
                .ok_or_else(|| anyhow::anyhow!("missing project ID"))?;
            ensure!(
                !fields.contains_key(&Key::project(id, "alive")),
                "duplicate project ID"
            );
            fields.insert(Key::project(id, "alive"), json!(true));
            for field in ["name", "color"] {
                fields.insert(
                    Key::project(id, field),
                    p.get(field).cloned().unwrap_or_else(|| match field {
                        "color" => json!("#64b5f6"),
                        _ => json!("Project"),
                    }),
                );
            }
            if let Some(folders) = p["folders"].as_array() {
                for f in folders {
                    let alias = f["machine_id"]
                        .as_str()
                        .filter(|s| !s.is_empty())
                        .or_else(|| f["machine"].as_str())
                        .unwrap_or_default();
                    // A local Windows folder is skipped whole. A peer's drive path, like
                    // other invalid paths, still fails when written.
                    let windows = f["path"].as_str().is_some_and(windows_folder);
                    if windows && machine_id(alias)? == local {
                        continue;
                    }
                    let folder = f["id"]
                        .as_str()
                        .ok_or_else(|| anyhow::anyhow!("missing folder ID"))?;
                    let machine = machine_id(alias)?;
                    let key = Key::Folder { id: folder.into() };
                    ensure!(!fields.contains_key(&key), "duplicate folder ID");
                    fields.insert(key,json!({"project":id,"machine":machine,"path":f["path"],"name":f.get("name").cloned().unwrap_or(json!(""))}));
                }
            }
            if let Some(sessions) = p["sessions"].as_array() {
                for s in sessions {
                    // The marker is local presentation, even if an importer or
                    // older UI bypassed the current desktop's payload filter.
                    if organization["unassigned_sessions"].as_array()
                        .is_some_and(|unassigned| unassigned.contains(s)) { continue; }
                    let (machine, session) = s
                        .as_str()
                        .and_then(|s| s.split_once('\n'))
                        .ok_or_else(|| anyhow::anyhow!("invalid session identity"))?;
                    let machine = machine_id(machine)?;
                    let key = Key::Session {
                        machine,
                        session: session.into(),
                    };
                    ensure!(
                        !fields.contains_key(&key),
                        "session belongs to more than one project"
                    );
                    fields.insert(key, json!(id));
                }
            }
        }
        // Only desktops that know the shared order send it; an older desktop's
        // local order stays local and never overrides the swarm's.
        if let Some(order) = organization.get("project_order") {
            fields.insert(Key::project_order(), order.clone());
        }
        Ok(fields)
    }

    pub fn apply(&mut self, patch: Patch) -> Result<()> {
        let before = self.flatten(&patch.base)?;
        let after = self.flatten(&patch.desired)?;
        let current = self.catalog.values();
        let observed: BTreeMap<Key, Vec<String>> = patch
            .versions
            .iter()
            .filter_map(|(token, ids)| {
                serde_json::from_str::<Key>(token)
                    .ok()
                    .map(|key| (self.catalog.canonical_key(&key), ids.clone()))
            })
            .fold(BTreeMap::new(), |mut result, (key, ids)| {
                result.entry(key).or_default().extend(ids);
                result
            });
        let keys: BTreeSet<_> = before.keys().chain(after.keys()).cloned().collect();
        for key in keys {
            if before.get(&key) == after.get(&key) {
                continue;
            }
            // Older desktops publish passive default placement as membership.
            // Keep their late observations from undoing shared assignments.
            // New desktops omit passive rows and identify explicit membership,
            // whose unseen concurrent edits must remain genuine conflicts.
            if matches!(key, Key::Session { .. })
                && !before.contains_key(&key)
                && current.contains_key(&key)
                && patch.desired["session_assignments_explicit"] != true
            {
                continue;
            }
            let value = match after.get(&key) {
                Some(value) => value.clone(),
                None => match &key {
                    Key::Project { field, .. } if field == "alive" => json!(false),
                    Key::Folder { .. } | Key::Session { .. } => Value::Null,
                    _ => continue,
                },
            };
            if matches!(key, Key::Folder { .. }) {
                if let Some(old) = before.get(&key).filter(|v| !v.is_null()) {
                    // The UI coalesces duplicate locations from independent imports.
                    // Editing/removing that location also removes the observed
                    // duplicates, so a hidden duplicate cannot reappear afterwards.
                    for (duplicate, location) in &current {
                        if duplicate == &key
                            || !matches!(duplicate, Key::Folder { .. })
                            || location.is_null()
                        {
                            continue;
                        }
                        let same = location["project"] == old["project"]
                            && location["path"] == old["path"]
                            && self
                                .catalog
                                .machine(location["machine"].as_str().unwrap_or_default())
                                == old["machine"].as_str().unwrap_or_default();
                        if same {
                            if let Some(parents) = observed.get(duplicate) {
                                self.catalog.write(
                                    &self.node_id,
                                    duplicate.clone(),
                                    Value::Null,
                                    parents.clone(),
                                )?;
                            }
                        }
                    }
                }
            }
            let parents = observed.get(&key).cloned().unwrap_or_default();
            self.catalog.write(&self.node_id, key, value, parents)?;
        }
        Ok(())
    }

    pub fn initialize(&mut self, organization: &Value) -> Result<()> {
        if self.imported {
            return Ok(());
        }
        let fields = self.flatten(organization)?;
        let existing = self.catalog.values();
        for (key, value) in fields {
            // The GUI may first open after this node joined. Import only fields
            // absent from the received catalog, never replay stale local values.
            if !existing.contains_key(&key) {
                self.catalog.write(&self.node_id, key, value, vec![])?;
            }
        }
        self.imported = true;
        Ok(())
    }

    pub fn snapshot(&self, config: &Config) -> Value {
        let values = self.catalog.values();
        let identities = self.catalog.identities().unwrap_or_default();
        let canonical = |id: &str| identities.get(id).cloned().unwrap_or_else(|| id.into());
        let name = |id: &str| {
            values
                .get(&Key::machine(id, "name"))
                .and_then(Value::as_str)
                .unwrap_or(id)
                .to_owned()
        };
        let mut aliases = BTreeMap::from([(self.node_id.clone(), config.host().to_owned())]);
        for alias in &config.peers {
            if let Some(binding) = self.bindings.get(alias) {
                aliases
                    .entry(canonical(&binding.id))
                    .or_insert_with(|| alias.clone());
            }
        }
        let mut projects: BTreeMap<String, Value> = BTreeMap::new();
        for (key, value) in &values {
            if let Key::Project { id, field } = key {
                if field == "alive" && value == true {
                    let field = |f| {
                        values
                            .get(&Key::project(id, f))
                            .cloned()
                            .unwrap_or(Value::Null)
                    };
                    projects.insert(id.clone(),json!({"id":id,"name":field("name"),"color":field("color"),"folders":[],"sessions":[],"accessible":false}));
                }
            }
        }
        for (key, value) in &values {
            match key {
                Key::Folder { id } if !value.is_null() => {
                    if let Some(p) = value["project"]
                        .as_str()
                        .and_then(|id| projects.get_mut(id))
                    {
                        let machine = canonical(value["machine"].as_str().unwrap_or_default());
                        let alias = aliases.get(&machine).cloned();
                        if alias.is_some() {
                            p["accessible"] = json!(true);
                        }
                        let folders = p["folders"].as_array_mut().unwrap();
                        // Independently imported folder locations can have distinct IDs.
                        // Keep both records durable; present the same location only once.
                        if !folders
                            .iter()
                            .any(|f| f["machine_id"] == machine && f["path"] == value["path"])
                        {
                            folders.push(json!({"id":id,"machine":alias.unwrap_or(machine.clone()),"machine_id":machine,"machine_name":name(&machine),"path":value["path"],"name":value["name"]}));
                        }
                    }
                }
                Key::Session { machine, session } if !value.is_null() => {
                    if let Some(p) = value.as_str().and_then(|id| projects.get_mut(id)) {
                        let alias = aliases.get(machine).cloned();
                        if alias.is_some() {
                            p["accessible"] = json!(true);
                        }
                        p["sessions"].as_array_mut().unwrap().push(json!(format!(
                            "{}\n{session}",
                            alias.unwrap_or(machine.clone())
                        )));
                    }
                }
                _ => {}
            }
        }
        for p in projects.values_mut() {
            if p["folders"].as_array().unwrap().is_empty()
                && p["sessions"].as_array().unwrap().is_empty()
            {
                p["accessible"] = json!(true);
            }
        }
        let conflicts: Vec<_> = self
            .catalog
            .effective_heads()
            .into_iter()
            // Concurrent preference and order edits resolve to the newest head
            // and the next choice on any device supersedes them; nothing to review.
            .filter(|(key, _)| !matches!(key, Key::Preference { .. } | Key::Order { .. }))
            .filter_map(|(key, heads)| {
                let variants: BTreeSet<_> = heads.iter().map(|o| o.value.to_string()).collect();
                if variants.len() <= 1 {
                    return None;
                }
                let selected = Catalog::selected(&key, &heads).map(|o| o.id.clone());
                Some(json!({"key":key.token(),"field":key,"selected":selected,"variants":heads}))
            })
            .collect();
        let machines: Vec<_> = values.keys().filter_map(|key| {
            let Key::Machine { id,field } = key else { return None; };
            if field != "name" || canonical(id) != *id { return None; }
            Some(json!({"id":id,"name":name(id),"connection":aliases.get(id),"local":id == &self.node_id}))
        }).collect();
        // The shared order first, then projects it does not name yet, by ID.
        let order = values.get(&Key::project_order());
        let mut ordered = Vec::with_capacity(projects.len());
        for id in order.and_then(Value::as_array).into_iter().flatten() {
            if let Some(project) = id.as_str().and_then(|id| projects.remove(id)) {
                ordered.push(project);
            }
        }
        ordered.extend(projects.into_values());
        let ids: Vec<Value> = ordered.iter().map(|p| p["id"].clone()).collect();
        let mut organization =
            json!({"version":2,"default_project":"ungrouped","projects":ordered});
        if order.is_some() {
            organization["project_order"] = Value::Array(ids);
        }
        json!({"schema":1,"node_id":self.node_id,"swarm_id":self.catalog.swarm_id,"digest":self.catalog.digest(),"initialized":self.imported,
            "organization":organization,
            "versions":self.catalog.versions(),"conflicts":conflicts,"preferences":self.catalog.preferences(),"machines":machines,"peers":self.peers})
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::swarm::{catalog::new_id, store::Binding};
    fn store() -> Store {
        let node_id = new_id();
        Store {
            node_id: node_id.clone(),
            catalog: Catalog::default(),
            imported: false,
            bindings: BTreeMap::from([(
                "local".into(),
                Binding {
                    id: node_id,
                    verified: true,
                },
            )]),
            peers: BTreeMap::new(),
        }
    }
    fn org(name: &str) -> Value {
        json!({"projects":[{"id":"project","name":name,"color":"#123456","vivid":false,"folders":[],"sessions":[]}]})
    }
    #[test]
    fn patches_use_observed_versions_and_do_not_overwrite_unseen_edits() {
        let mut a = store();
        a.initialize(&org("Initial")).unwrap();
        let versions = a.catalog.versions();
        let mut b = a.clone();
        b.node_id = new_id();
        b.apply(Patch {
            base: org("Initial"),
            desired: org("Peer edit"),
            versions: versions.clone(),
        })
        .unwrap();
        a.catalog
            .merge(
                &b.catalog.swarm_id,
                b.catalog.operations.into_values().collect(),
            )
            .unwrap();
        a.apply(Patch {
            base: org("Initial"),
            desired: org("Local edit"),
            versions,
        })
        .unwrap();
        assert_eq!(a.catalog.heads()[&Key::project("project", "name")].len(), 2);
    }
    #[test]
    fn local_preferences_are_not_exported_and_import_is_idempotent() {
        let mut a = store();
        let mut document = org("Initial");
        document["default_project"] = json!("project");
        document["ssh_private_key"] = json!("SECRET");
        document["projects"][0]["collapsed"] = json!(true);
        a.initialize(&document).unwrap();
        let before = a.catalog.digest();
        a.initialize(&org("Outdated GUI cache")).unwrap();
        assert_eq!(a.catalog.digest(), before);
        let wire = serde_json::to_string(&a.catalog).unwrap();
        for private in ["SECRET", "collapsed", "default_project", "ssh_private_key"] {
            assert!(!wire.contains(private));
        }
    }
    #[test]
    fn fallback_markers_never_become_shared_assignments_even_on_import() {
        let mut a = store();
        let mut document = org("Initial");
        document["projects"][0]["sessions"] = json!(["local\ncodex/work/new"]);
        document["unassigned_sessions"] = json!(["local\ncodex/work/new"]);
        a.initialize(&document).unwrap();
        assert!(!a.catalog.values().keys().any(|key| matches!(key, Key::Session { .. })));
        let before = a.snapshot(&config());
        a.apply(Patch { base: before["organization"].clone(), desired: document, versions: a.catalog.versions() }).unwrap();
        assert!(!a.catalog.values().keys().any(|key| matches!(key, Key::Session { .. })));
    }
    #[test]
    fn observing_a_session_does_not_undo_a_remote_project_assignment() {
        let mut a = store();
        a.initialize(&org("Initial")).unwrap();
        let versions = a.catalog.versions();
        let key = Key::Session {
            machine: a.node_id.clone(),
            session: "codex/test/one".into(),
        };
        a.catalog
            .set(&new_id(), key.clone(), json!("another-project"))
            .unwrap();
        let mut desired = org("Initial");
        desired["projects"][0]["sessions"] = json!(["local\ncodex/test/one"]);
        a.apply(Patch {
            base: org("Initial"),
            desired,
            versions,
        })
        .unwrap();
        assert_eq!(a.catalog.values()[&key], "another-project");
    }
    #[test]
    fn explicit_initial_assignment_preserves_an_unseen_competing_choice() {
        let mut a = store();
        a.initialize(&org("Initial")).unwrap();
        let versions = a.catalog.versions();
        let key = Key::Session {
            machine: a.node_id.clone(),
            session: "codex/test/one".into(),
        };
        a.catalog
            .set(&new_id(), key.clone(), json!("another-project"))
            .unwrap();
        let mut desired = org("Initial");
        desired["projects"][0]["sessions"] = json!(["local\ncodex/test/one"]);
        desired["session_assignments_explicit"] = json!(true);
        a.apply(Patch {
            base: org("Initial"),
            desired,
            versions,
        })
        .unwrap();
        let heads = a.catalog.heads();
        assert_eq!(heads[&key].len(), 2);
        let values: BTreeSet<_> = heads[&key]
            .iter()
            .map(|op| op.value.as_str().unwrap())
            .collect();
        assert_eq!(values, BTreeSet::from(["project", "another-project"]));
    }
    #[test]
    fn windows_folder_paths_are_drive_or_unc() {
        for path in [
            "C:\\work",
            "c:/",
            "D:\\",
            "C:\\work/sub\\dir",
            "\\\\server\\share",
            "//server/share/dir",
            "\\/server/share",
            "\\\\?\\C:\\work",
        ] {
            assert!(windows_folder(path), "{path}");
        }
        for path in [
            "/work",
            "/",
            "//server",
            "//server/",
            "//server//share",
            "///share",
            "C:",
            "C:work",
            "1:/work",
            "/work/C:/x",
            "work",
            "",
        ] {
            assert!(!windows_folder(path), "{path}");
        }
    }
    #[test]
    fn local_windows_folders_stay_local_and_other_invalid_paths_still_fail() {
        let mut a = store();
        a.initialize(&org("Initial")).unwrap();
        let versions = a.catalog.versions();
        let digest = a.catalog.digest();
        let with = |folder: Value| {
            let mut document = org("Initial");
            document["projects"][0]["folders"] = json!([folder]);
            document
        };
        // This machine by its alias, or by its ID once a snapshot named it.
        let node = a.node_id.clone();
        let local = |path: &str| {
            [
                with(json!({"id":"local-folder","machine":"local","path":path,"name":""})),
                with(
                    json!({"id":"local-folder","machine":"desktop","machine_id":node,"path":path,"name":""}),
                ),
            ]
        };
        for path in [
            "C:\\work",
            "c:/Users/user/project",
            "\\\\server\\share\\dir",
            "//server/share",
        ] {
            for document in local(path) {
                a.apply(Patch {
                    base: org("Initial"),
                    desired: document.clone(),
                    versions: versions.clone(),
                })
                .unwrap();
                a.apply(Patch {
                    base: document,
                    desired: org("Initial"),
                    versions: versions.clone(),
                })
                .unwrap();
                assert_eq!(a.catalog.digest(), digest, "{path}");
            }
        }
        for document in local("C:\\work") {
            assert!(a
                .flatten(&document)
                .unwrap()
                .keys()
                .all(|key| !matches!(key, Key::Folder { .. })));
        }
        let [work, work_by_id] = local("work");
        let [drive_relative, _] = local("C:work");
        let [rooted, _] = local("\\work");
        let peer = new_id();
        for desired in [
            work,
            work_by_id,
            drive_relative,
            rooted,
            // A peer's drive path, by alias or ID, is no folder of a POSIX machine.
            with(json!({"id":"peer-folder","machine":"laptop","path":"C:/x","name":""})),
            with(
                json!({"id":"peer-folder","machine":"laptop","machine_id":peer,"path":"c:\\work","name":""}),
            ),
        ] {
            assert!(
                a.apply(Patch {
                    base: org("Initial"),
                    desired: desired.clone(),
                    versions: versions.clone(),
                })
                .is_err(),
                "{desired}"
            );
        }
        a.apply(Patch {
            base: org("Initial"),
            desired: local("/work")[0].clone(),
            versions,
        })
        .unwrap();
        assert_eq!(
            a.catalog.values()[&Key::Folder {
                id: "local-folder".into()
            }]["path"],
            "/work"
        );
    }
    #[test]
    fn a_peers_unc_shaped_folder_is_a_posix_path() {
        // A Windows GUI can store "//server/share" for a POSIX peer.
        let mut a = store();
        a.initialize(&org("Initial")).unwrap();
        let peer = a.machine_id("laptop").unwrap();
        let key = Key::Folder {
            id: "peer-folder".into(),
        };
        let with = |machine: &str, name: &str| {
            let mut document = org("Initial");
            document["projects"][0]["folders"] = json!([{"id":"peer-folder","machine":machine,"path":"//server/share/dir","name":name}]);
            document
        };
        let mut edit = |base: Value, desired: Value| {
            let versions = a.catalog.versions();
            a.apply(Patch {
                base,
                desired,
                versions,
            })
            .unwrap();
            a.catalog.values().get(&key).cloned().unwrap_or(Value::Null)
        };
        let written = edit(org("Initial"), with("laptop", ""));
        assert_eq!(written["path"], "//server/share/dir");
        assert_eq!(written["machine"], peer.as_str());
        // A snapshot names the peer by its ID; renaming and removing it sync.
        assert_eq!(
            edit(with("laptop", ""), with(&peer, "Share"))["name"],
            "Share"
        );
        assert!(edit(with(&peer, "Share"), org("Initial")).is_null());
        assert_eq!(a.catalog.heads()[&key].len(), 1);
    }
    #[test]
    fn verified_connection_cannot_silently_change_machine_identity() {
        let mut a = store();
        let placeholder = a.machine_id("peer").unwrap();
        let real = new_id();
        a.bind("peer", &real).unwrap();
        assert_eq!(a.catalog.machine(&placeholder), real);
        assert!(a.bind("peer", &new_id()).is_err());
    }
    fn config() -> Config {
        Config {
            home: "/home/example".into(),
            dir: "/home/example/.config/hgs".into(),
            state: "/home/example/.local/state/hgs".into(),
            selves: vec!["local".into()],
            peers: vec![],
            base_peers: vec![],
            peers_from_environment: false,
            machines: BTreeMap::new(),
            tab: false,
            tab_colors: String::new(),
        }
    }
    fn projects(ids: &[&str]) -> Value {
        let projects: Vec<_> = ids
            .iter()
            .map(|id| json!({"id":id,"name":id.to_uppercase(),"color":"#123456","folders":[],"sessions":[]}))
            .collect();
        json!({ "projects": projects })
    }
    fn shown(store: &Store) -> (Vec<String>, Option<Value>) {
        let organization = store.snapshot(&config())["organization"].clone();
        let ids = organization["projects"].as_array().unwrap().iter();
        let ids = ids.map(|p| p["id"].as_str().unwrap().to_owned()).collect();
        (ids, organization.get("project_order").cloned())
    }
    #[test]
    fn the_shared_project_order_orders_snapshots_and_unnamed_projects_follow_by_id() {
        let mut a = store();
        a.initialize(&projects(&["b", "c", "a"])).unwrap();
        // Without a shared order the snapshot keeps its ID order and says so.
        assert_eq!(shown(&a), (vec!["a".into(), "b".into(), "c".into()], None));
        let before = a.snapshot(&config());
        let mut desired = projects(&["c", "a", "b"]);
        desired["project_order"] = json!(["c", "a", "b"]);
        a.apply(Patch {
            base: before["organization"].clone(),
            desired,
            versions: a.catalog.versions(),
        })
        .unwrap();
        let order = |ids: &[&str]| {
            (
                ids.iter().map(|id| id.to_string()).collect(),
                Some(json!(ids)),
            )
        };
        assert_eq!(shown(&a), order(&["c", "a", "b"]));
        // A project the order does not name yet follows it; a removed one leaves it.
        a.catalog
            .set(&a.node_id.clone(), Key::project("0", "alive"), json!(true))
            .unwrap();
        a.catalog
            .set(&a.node_id.clone(), Key::project("a", "alive"), json!(false))
            .unwrap();
        assert_eq!(shown(&a), order(&["c", "b", "0"]));
        // Republishing the same order writes nothing.
        let before = a.snapshot(&config());
        let operations = a.catalog.operations.len();
        a.apply(Patch {
            base: before["organization"].clone(),
            desired: before["organization"].clone(),
            versions: a.catalog.versions(),
        })
        .unwrap();
        assert_eq!(a.catalog.operations.len(), operations);
    }
    #[test]
    fn a_desktop_without_the_shared_order_never_changes_it() {
        let mut a = store();
        let mut initial = projects(&["a", "b"]);
        initial["project_order"] = json!(["b", "a"]);
        a.initialize(&initial).unwrap();
        assert_eq!(shown(&a).1, Some(json!(["b", "a"])));
        // An older desktop keeps the snapshot's field in its base and drops it
        // from its desired local order, or omits it from both.
        let before = a.snapshot(&config());
        let base = before["organization"].clone();
        for (base, desired) in [
            (base.clone(), projects(&["a", "b"])),
            (projects(&["b", "a"]), projects(&["a", "b"])),
        ] {
            a.apply(Patch {
                base,
                desired,
                versions: a.catalog.versions(),
            })
            .unwrap();
            assert_eq!(shown(&a).1, Some(json!(["b", "a"])));
        }
        assert_eq!(a.catalog.heads()[&Key::project_order()].len(), 1);
    }
    #[test]
    fn concurrent_project_orders_settle_on_the_newest_without_a_review() {
        let mut a = store();
        a.initialize(&projects(&["a", "b", "c"])).unwrap();
        let mut b = a.clone();
        b.node_id = new_id();
        let base = a.snapshot(&config())["organization"].clone();
        for (store, order) in [(&mut a, ["c", "b", "a"]), (&mut b, ["b", "a", "c"])] {
            let mut desired = projects(&order);
            desired["project_order"] = json!(order);
            store
                .apply(Patch {
                    base: base.clone(),
                    desired,
                    versions: store.catalog.versions(),
                })
                .unwrap();
        }
        let operations: Vec<_> = b.catalog.operations.values().cloned().collect();
        a.catalog.merge(&b.catalog.swarm_id, operations).unwrap();
        let operations: Vec<_> = a.catalog.operations.values().cloned().collect();
        b.catalog.merge(&a.catalog.swarm_id, operations).unwrap();
        assert_eq!(a.catalog.heads()[&Key::project_order()].len(), 2);
        assert_eq!(shown(&a), shown(&b));
        assert_eq!(a.snapshot(&config())["conflicts"], json!([]));
    }
}
