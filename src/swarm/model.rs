use super::{
    catalog::{Catalog, Key},
    store::Store,
};
use crate::config::Config;
use anyhow::{ensure, Result};
use serde::Deserialize;
use serde_json::{json, Value};
use std::collections::{BTreeMap, BTreeSet};

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Patch {
    pub base: Value,
    pub desired: Value,
    pub versions: BTreeMap<String, Vec<String>>,
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
    fn verified_connection_cannot_silently_change_machine_identity() {
        let mut a = store();
        let placeholder = a.machine_id("peer").unwrap();
        let real = new_id();
        a.bind("peer", &real).unwrap();
        assert_eq!(a.catalog.machine(&placeholder), real);
        assert!(a.bind("peer", &new_id()).is_err());
    }
}

impl Store {
    pub fn flatten(&mut self, organization: &Value) -> Result<BTreeMap<Key, Value>> {
        let mut fields = BTreeMap::new();
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
                    let folder = f["id"]
                        .as_str()
                        .ok_or_else(|| anyhow::anyhow!("missing folder ID"))?;
                    let machine = machine_id(
                        f["machine_id"]
                            .as_str()
                            .filter(|s| !s.is_empty())
                            .or_else(|| f["machine"].as_str())
                            .unwrap_or_default(),
                    )?;
                    let key = Key::Folder { id: folder.into() };
                    ensure!(!fields.contains_key(&key), "duplicate folder ID");
                    fields.insert(key,json!({"project":id,"machine":machine,"path":f["path"],"name":f.get("name").cloned().unwrap_or(json!(""))}));
                }
            }
            if let Some(sessions) = p["sessions"].as_array() {
                for s in sessions {
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
            // Seeing an unknown session in a fleet snapshot gives it the local
            // default project. This observation must not move an existing shared
            // membership that arrived since the GUI's previous snapshot.
            if matches!(key, Key::Session { .. })
                && !before.contains_key(&key)
                && current.contains_key(&key)
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
            .heads()
            .into_iter()
            // Concurrent preference edits resolve to the newest head and the
            // next choice on any device supersedes them; nothing to review.
            .filter(|(key, _)| !matches!(key, Key::Preference { .. }))
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
        json!({"schema":1,"node_id":self.node_id,"swarm_id":self.catalog.swarm_id,"digest":self.catalog.digest(),"initialized":self.imported,
            "organization":{"version":2,"default_project":"ungrouped","projects":projects.into_values().collect::<Vec<_>>()},
            "versions":self.catalog.versions(),"conflicts":conflicts,"preferences":self.catalog.preferences(),"machines":machines,"peers":self.peers})
    }
}
