//! Exact logical session grouping, independent of launch folders or providers.
//! Reads never initialize/sync a catalog. A durable unfinished assignment is
//! uncertain forever; neither a duplicate nor a fresh membership read replays it.
use super::{
    catalog::Key,
    launch_project::{check_capacity, write_receipts, Receipts},
    store::{LockedStore, Store},
};
use crate::config::Config;
use anyhow::{ensure, Result};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::{collections::BTreeMap, path::Path};

const PAGE: usize = 100;
const MAX_PROJECTS: usize = 5000;

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Request {
    request_id: String,
    name: String,
    expected_run_id: String,
    expected_conversation_id: String,
    expected_machine_id: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    cursor: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    swarm_id: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    membership_token: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    catalog_epoch: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    project_id: Option<String>,
}
fn uuid(id: &str) -> bool {
    uuid::Uuid::parse_str(id).is_ok_and(|u| !u.is_nil() && u.to_string() == id)
}
fn label(s: &str, max: usize, nonempty: bool) -> bool {
    (!nonempty || !s.is_empty()) && s.len() <= max && !s.chars().any(char::is_control)
}
fn hex(s: &str) -> bool {
    s.len() == 64
        && s.bytes()
            .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
}
fn hash(value: &Value) -> String {
    format!(
        "{:x}",
        Sha256::digest(serde_json::to_vec(value).expect("JSON value"))
    )
}
impl Request {
    fn parse(value: &Value, mutation: bool) -> Result<Self> {
        let base = [
            "request_id",
            "name",
            "expected_run_id",
            "expected_conversation_id",
            "expected_machine_id",
        ];
        let fields = [
            "swarm_id",
            "membership_token",
            "catalog_epoch",
            "project_id",
        ];
        let object = value
            .as_object()
            .ok_or_else(|| anyhow::anyhow!("Project request must be an object"))?;
        ensure!(
            base.iter().all(|key| object.contains_key(*key))
                && object.keys().all(|key| base.contains(&key.as_str())
                    || if mutation {
                        fields.contains(&key.as_str())
                    } else {
                        key == "cursor"
                    })
                && (!mutation || fields.iter().all(|key| object.contains_key(*key)))
                && object.values().all(Value::is_string),
            "Unexpected project request fields"
        );
        let request: Self = serde_json::from_value(value.clone())?;
        request.validate(mutation)?;
        Ok(request)
    }
    fn validate(&self, mutation: bool) -> Result<()> {
        ensure!(uuid(&self.request_id), "Invalid request UUID");
        ensure!(
            uuid(&self.expected_machine_id),
            "Invalid expected native computer UUID"
        );
        ensure!(label(&self.name, 2048, true), "Invalid native session name");
        ensure!(
            label(&self.expected_run_id, 128, true),
            "Invalid expected run ID"
        );
        ensure!(
            label(&self.expected_conversation_id, 2048, false),
            "Invalid expected conversation ID"
        );
        if mutation {
            ensure!(
                self.cursor.is_none(),
                "Assignments do not accept a catalog cursor"
            );
            ensure!(
                self.swarm_id.as_deref().is_some_and(uuid),
                "Invalid expected swarm UUID"
            );
            ensure!(
                self.membership_token.as_deref().is_some_and(hex),
                "Invalid membership token"
            );
            ensure!(
                self.catalog_epoch.as_deref().is_some_and(hex),
                "Invalid catalog epoch"
            );
            ensure!(
                self.project_id
                    .as_deref()
                    .is_some_and(|id| label(id, 128, true)),
                "Invalid project ID"
            );
        } else {
            ensure!(
                self.swarm_id.is_none()
                    && self.membership_token.is_none()
                    && self.catalog_epoch.is_none()
                    && self.project_id.is_none(),
                "Read does not accept assignment fields"
            );
            ensure!(
                self.cursor.as_deref().is_none_or(|s| label(s, 512, true)),
                "Invalid project cursor"
            );
        }
        Ok(())
    }
    fn check_binding(&self, record: &Value) -> Result<()> {
        ensure!(
            record["name"] == self.name
                && record["run_id"] == self.expected_run_id
                && record["conversation_id"].as_str().unwrap_or_default()
                    == self.expected_conversation_id,
            "Session identity changed; no replacement was selected"
        );
        ensure!(
            ["parent_session", "parent_session_id"]
                .iter()
                .all(|key| record
                    .get(*key)
                    .is_none_or(|value| value.is_null() || value.as_str() == Some(""))),
            "Choose the parent session for project grouping"
        );
        Ok(())
    }
    fn outcome(&self, status: &str) -> Value {
        let mut result = json!({"schema":1,"request_id":self.request_id,"status":status,
            "name":self.name,"run_id":self.expected_run_id,"conversation_id":self.expected_conversation_id,
            "machine_id":self.expected_machine_id});
        if let Some(id) = &self.swarm_id {
            result["swarm_id"] = json!(id);
        }
        if let Some(id) = &self.project_id {
            result["project_id"] = json!(id);
        }
        if let Some(token) = &self.membership_token {
            result["membership_token"] = json!(token);
        }
        if let Some(epoch) = &self.catalog_epoch {
            result["catalog_epoch"] = json!(epoch);
        }
        result
    }
}
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Project {
    id: String,
    name: String,
    color: String,
}
struct Snapshot {
    key: Key,
    current: String,
    membership: String,
    epoch: String,
    projects: Vec<Project>,
}
fn snapshot(store: &Store, request: &Request) -> Result<Snapshot> {
    ensure!(
        store.imported && uuid(&store.node_id) && store.node_id == request.expected_machine_id,
        "Native computer or initialized project catalog changed"
    );
    let values = store.catalog.values();
    let key = Key::Session {
        machine: store.catalog.machine(&store.node_id),
        session: request.name.clone(),
    };
    let current = values
        .get(&key)
        .and_then(Value::as_str)
        .unwrap_or("ungrouped")
        .to_owned();
    let heads = store
        .catalog
        .versions()
        .remove(&key.token())
        .unwrap_or_default();
    let membership = hash(
        &json!({"schema":1,"swarm_id":store.catalog.swarm_id,"machine_id":store.node_id,
        "key":key,"run_id":request.expected_run_id,"conversation_id":request.expected_conversation_id,"heads":heads,"current":current}),
    );
    let mut projects = BTreeMap::new();
    for (key, value) in &values {
        if let Key::Project { id, field } = key {
            if field == "alive" && value == true {
                ensure!(
                    projects.len() < MAX_PROJECTS,
                    "Project catalog exceeds the supported bound"
                );
                let name = values
                    .get(&Key::project(id, "name"))
                    .and_then(Value::as_str)
                    .ok_or_else(|| {
                        anyhow::anyhow!("A live project has no literal name; refresh the catalog")
                    })?;
                let color = values
                    .get(&Key::project(id, "color"))
                    .and_then(Value::as_str)
                    .ok_or_else(|| {
                        anyhow::anyhow!("A live project has no color; refresh the catalog")
                    })?;
                projects.insert(
                    id.clone(),
                    Project {
                        id: id.clone(),
                        name: name.to_owned(),
                        color: color.to_owned(),
                    },
                );
            }
        }
    }
    let mut ordered = Vec::with_capacity(projects.len());
    if let Some(order) = values.get(&Key::project_order()).and_then(Value::as_array) {
        for id in order.iter().filter_map(Value::as_str) {
            if let Some(project) = projects.remove(id) {
                ordered.push(project);
            }
        }
    }
    ordered.extend(projects.into_values());
    let epoch = hash(&json!({"schema":1,"swarm_id":store.catalog.swarm_id,"projects":ordered}));
    Ok(Snapshot {
        key,
        current,
        membership,
        epoch,
        projects: ordered,
    })
}
fn page(request: &Request, store: &Store, snapshot: &Snapshot) -> Result<Value> {
    let offset = if let Some(cursor) = &request.cursor {
        let (epoch, offset) = cursor
            .split_once(':')
            .ok_or_else(|| anyhow::anyhow!("Invalid project cursor"))?;
        ensure!(
            epoch == snapshot.epoch,
            "Project catalog changed; reopen project choices"
        );
        let number: usize = offset.parse()?;
        ensure!(
            number > 0
                && number.is_multiple_of(PAGE)
                && number.to_string() == offset
                && number < snapshot.projects.len(),
            "Invalid project page"
        );
        number
    } else {
        0
    };
    let end = (offset + PAGE).min(snapshot.projects.len());
    let mut result = request.outcome("ok");
    result["swarm_id"] = json!(store.catalog.swarm_id);
    result["current_project_id"] = json!(snapshot.current);
    result["membership_token"] = json!(snapshot.membership);
    result["catalog_epoch"] = json!(snapshot.epoch);
    result["projects"] = json!(&snapshot.projects[offset..end]);
    result["next_cursor"] = if end < snapshot.projects.len() {
        json!(format!("{}:{end}", snapshot.epoch))
    } else {
        Value::Null
    };
    Ok(result)
}
fn receipts(path: &Path) -> Result<Receipts> {
    match std::fs::symlink_metadata(path) {
        Ok(metadata) => ensure!(
            metadata.file_type().is_file(),
            "Assignment receipts are not a regular file"
        ),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(Receipts::new()),
        Err(error) => return Err(error.into()),
    }
    // Read this opened file once. A raced removal must not become the generic
    // launch reader's NotFound-as-empty admission.
    use std::io::Read;
    let file = std::fs::File::open(path)?;
    let mut bytes = Vec::new();
    file.take(128 * 1024 * 1024 + 1).read_to_end(&mut bytes)?;
    ensure!(
        bytes.len() <= 128 * 1024 * 1024,
        "Assignment journal is too large"
    );
    let receipts: Receipts = serde_json::from_slice(&bytes)?;
    ensure!(
        receipts.len() <= 10_000,
        "Assignment journal has too many receipts"
    );
    Ok(receipts)
}
fn recorded(receipts: &Receipts, request: &Request) -> Result<Option<Value>> {
    let Some(receipt) = receipts.get(&request.request_id) else {
        return Ok(None);
    };
    ensure!(
        receipt["kind"] == "session-project-v1"
            && receipt["request"] == serde_json::to_value(request)?,
        "Request UUID already belongs to another assignment intent"
    );
    ensure!(
        receipt["phase"] == "attempting" || receipt["phase"] == "terminal",
        "Invalid assignment receipt phase"
    );
    let result = receipt["outcome"].clone();
    let expected = request.outcome(result["status"].as_str().unwrap_or_default());
    ensure!(
        matches!(
            result["status"].as_str(),
            Some("assigned" | "failed" | "uncertain")
        ) && expected
            .as_object()
            .unwrap()
            .iter()
            .all(|(key, value)| result.get(key) == Some(value)),
        "Assignment receipt identity is damaged"
    );
    let object = result
        .as_object()
        .ok_or_else(|| anyhow::anyhow!("Invalid assignment outcome"))?;
    ensure!(
        object.keys().all(|key| expected.get(key).is_some()
            || ["error", "current_project_id"].contains(&key.as_str()))
            && if result["status"] == "assigned" {
                result["current_project_id"] == request.project_id.as_deref().unwrap_or_default()
                    && result.get("error").is_none()
            } else {
                result["error"]
                    .as_str()
                    .is_some_and(|error| label(error, 2048, true))
            },
        "Invalid assignment outcome fields"
    );
    if receipt["phase"] == "attempting" {
        let mut uncertain = request.outcome("uncertain");
        uncertain["error"] =
            json!("Assignment intent has no confirmed outcome; it will not be applied again");
        return Ok(Some(uncertain));
    }
    Ok(Some(result))
}
fn assign(store: &mut Store, request: &Request, snapshot: &Snapshot) -> Result<()> {
    ensure!(
        request.swarm_id.as_deref() == Some(store.catalog.swarm_id.as_str()),
        "Project swarm changed"
    );
    ensure!(
        request.membership_token.as_deref() == Some(snapshot.membership.as_str()),
        "Session project membership changed; review fresh choices"
    );
    ensure!(
        request.catalog_epoch.as_deref() == Some(snapshot.epoch.as_str()),
        "Project catalog changed; review fresh choices"
    );
    let id = request.project_id.as_deref().unwrap();
    ensure!(
        snapshot.projects.iter().any(|project| project.id == id),
        "Chosen logical project is no longer live"
    );
    store
        .catalog
        .set(&store.node_id, snapshot.key.clone(), json!(id))?;
    store.catalog.validate()
}
#[cfg(unix)]
pub(crate) fn validate_payload(value: &Value, mutation: bool) -> Result<()> {
    Request::parse(value, mutation).map(|_| ())
}
/// The catalog lock stays with the caller. The durable intent precedes the
/// sole supplied persistence call; a missing terminal receipt never authorizes
/// another persistence call. This same path is exercised by fault contracts.
fn commit_assignment(
    path: &Path,
    receipts: &mut Receipts,
    request: &Request,
    persist: impl FnOnce() -> Result<()>,
) -> Result<Value> {
    if let Some(outcome) = recorded(receipts, request)? {
        return Ok(outcome);
    }
    check_capacity(receipts)?;
    let mut uncertain = request.outcome("uncertain");
    uncertain["error"] =
        json!("Assignment outcome could not be confirmed; this UUID will not be applied again");
    receipts.insert(request.request_id.clone(), json!({"kind":"session-project-v1","request":request,"phase":"attempting","outcome":uncertain}));
    write_receipts(path, receipts)?;
    if let Err(error) = persist() {
        uncertain["error"] = json!(format!("Assignment persistence is uncertain: {error}"));
        return Ok(uncertain);
    }
    let mut result = request.outcome("assigned");
    result["current_project_id"] = json!(request.project_id);
    receipts.insert(
        request.request_id.clone(),
        json!({"kind":"session-project-v1","request":request,"phase":"terminal","outcome":result}),
    );
    if let Err(error) = write_receipts(path, receipts) {
        uncertain["error"] = json!(format!(
            "Assignment receipt persistence is uncertain: {error}"
        ));
        return Ok(uncertain);
    }
    Ok(result)
}

pub(super) fn execute(config: &Config, value: Value, mutation: bool) -> Value {
    let request = match Request::parse(&value, mutation) {
        Ok(request) => request,
        Err(_) => {
            return json!({"schema":1,"request_id":value["request_id"].as_str().unwrap_or_default(),"status":"failed","error":"Invalid project assignment request"})
        }
    };
    let attempted = (|| -> Result<Value> {
        // Duplicates are checked before current run/membership, so a later
        // manual move or native clear can never cause a previous UUID to run again.
        if mutation {
            let store = LockedStore::load_scoped(config)?;
            ensure!(
                store.data.node_id == request.expected_machine_id && uuid(&store.data.node_id),
                "Native computer identity changed"
            );
            let journal = store.path.with_file_name("launch-assignments.json");
            if let Some(result) = recorded(&receipts(&journal)?, &request)? {
                return Ok(result);
            }
        }
        crate::state::with_session_project_binding(&request.name, |record| {
            let mut store = LockedStore::load_scoped(config)?;
            if !mutation {
                request.check_binding(record)?;
                return page(&request, &store.data, &snapshot(&store.data, &request)?);
            }
            ensure!(
                store.data.node_id == request.expected_machine_id && uuid(&store.data.node_id),
                "Native computer identity changed"
            );
            let journal = store.path.with_file_name("launch-assignments.json");
            let mut receipts = receipts(&journal)?;
            if let Some(result) = recorded(&receipts, &request)? {
                return Ok(result);
            }
            check_capacity(&receipts)?;
            let prepared = (|| -> Result<()> {
                request.check_binding(record)?;
                let current = snapshot(&store.data, &request)?;
                assign(&mut store.data, &request, &current)
            })();
            if let Err(error) = prepared {
                // A definite pre-write refusal is durable too. The same UUID
                // cannot become eligible later and unexpectedly apply then.
                let mut result = request.outcome("failed");
                result["error"] = json!(error.to_string());
                receipts.insert(request.request_id.clone(), json!({"kind":"session-project-v1","request":request,"phase":"terminal","outcome":result}));
                write_receipts(&journal, &receipts)?;
                return Ok(result);
            }
            commit_assignment(&journal, &mut receipts, &request, || {
                store.save_catalog_only()
            })
        })
    })();
    match attempted {
        Ok(result) => result,
        Err(error) => {
            let mut result = request.outcome(if mutation { "uncertain" } else { "failed" });
            result["error"] = json!(error.to_string());
            result
        }
    }
}

#[cfg(test)]
mod tests {
    use super::super::catalog::{new_id, Catalog};
    use super::*;
    fn fixture() -> (Store, Request) {
        let mut store = Store {
            node_id: new_id(),
            catalog: Catalog::default(),
            imported: true,
            bindings: BTreeMap::new(),
            peers: BTreeMap::new(),
        };
        for (id, name) in [
            ("ungrouped", "Inbox"),
            ("one", "One"),
            ("two", "Два ' $(literal)"),
        ] {
            for (field, value) in [
                ("alive", json!(true)),
                ("name", json!(name)),
                ("color", json!("#123456")),
            ] {
                store
                    .catalog
                    .set(&store.node_id, Key::project(id, field), value)
                    .unwrap();
            }
        }
        let request = Request {
            request_id: new_id(),
            name: "claude/example/task".into(),
            expected_run_id: "run-1".into(),
            expected_conversation_id: "one".into(),
            expected_machine_id: store.node_id.clone(),
            cursor: None,
            swarm_id: None,
            membership_token: None,
            catalog_epoch: None,
            project_id: None,
        };
        (store, request)
    }
    fn mutation(store: &Store, query: &Request) -> Request {
        let snapshot = snapshot(store, query).unwrap();
        Request {
            swarm_id: Some(store.catalog.swarm_id.clone()),
            membership_token: Some(snapshot.membership),
            catalog_epoch: Some(snapshot.epoch),
            project_id: Some("two".into()),
            ..query.clone()
        }
    }
    #[test]
    fn logical_assignment_changes_only_the_exact_session_register() {
        let (mut store, query) = fixture();
        let unrelated = Key::Session {
            machine: new_id(),
            session: query.name.clone(),
        };
        store
            .catalog
            .set(&store.node_id, unrelated.clone(), json!("one"))
            .unwrap();
        let request = mutation(&store, &query);
        let before = serde_json::to_value(&store).unwrap();
        let snapshot = snapshot(&store, &request).unwrap();
        assign(&mut store, &request, &snapshot).unwrap();
        let after = serde_json::to_value(&store).unwrap();
        for field in ["node_id", "bindings", "peers", "imported"] {
            assert_eq!(before[field], after[field]);
        }
        let added: Vec<_> = store
            .catalog
            .operations
            .iter()
            .filter(|(id, _)| before["catalog"]["operations"].get(id.as_str()).is_none())
            .collect();
        assert_eq!(added.len(), 1);
        assert_eq!(added[0].1.key, snapshot.key);
        assert_eq!(added[0].1.value, "two");
        assert_eq!(store.catalog.values()[&unrelated], "one");
        for (id, operation) in before["catalog"]["operations"].as_object().unwrap() {
            assert_eq!(after["catalog"]["operations"][id], *operation);
        }
    }
    #[test]
    fn wrong_run_conversation_machine_swarm_membership_and_epoch_refuse() {
        let (mut store, query) = fixture();
        let request = mutation(&store, &query);
        let record = json!({"name":query.name,"run_id":"run-1","conversation_id":"one"});
        request.check_binding(&record).unwrap();
        for field in ["name", "run_id", "conversation_id"] {
            let mut bad = record.clone();
            bad[field] = json!("foreign");
            assert!(request.check_binding(&bad).is_err());
        }
        for field in ["parent_session", "parent_session_id"] {
            let mut bad = record.clone();
            bad[field] = json!("parent");
            assert!(request.check_binding(&bad).is_err());
        }
        let mut wrong = request.clone();
        wrong.expected_machine_id = new_id();
        assert!(snapshot(&store, &wrong).is_err());
        for field in [
            "swarm_id",
            "membership_token",
            "catalog_epoch",
            "project_id",
        ] {
            let mut raw = serde_json::to_value(&request).unwrap();
            raw[field] = json!("foreign");
            let bad: Request = serde_json::from_value(raw).unwrap();
            let before = serde_json::to_value(&store).unwrap();
            let current = snapshot(&store, &request).unwrap();
            assert!(assign(&mut store, &bad, &current).is_err());
            assert_eq!(serde_json::to_value(&store).unwrap(), before);
        }
    }
    #[test]
    fn catalog_changes_and_membership_aba_invalidate_the_original_intent() {
        let (mut store, query) = fixture();
        let request = mutation(&store, &query);
        store
            .catalog
            .set(
                &store.node_id,
                Key::project("two", "name"),
                json!("Renamed"),
            )
            .unwrap();
        let current = snapshot(&store, &request).unwrap();
        assert!(assign(&mut store, &request, &current).is_err());
        let request = mutation(&store, &query);
        let key = snapshot(&store, &query).unwrap().key;
        store
            .catalog
            .set(&store.node_id, key.clone(), json!("one"))
            .unwrap();
        store.catalog.set(&store.node_id, key, Value::Null).unwrap();
        let current = snapshot(&store, &request).unwrap();
        assert!(assign(&mut store, &request, &current).is_err());
    }
    #[test]
    fn bounded_pages_do_not_mix_epochs_and_literal_projects_need_no_folder() {
        let (mut store, query) = fixture();
        for index in 0..105 {
            let id = format!("p{index:03}");
            for (field, value) in [
                ("alive", json!(true)),
                ("name", json!(id)),
                ("color", json!("#abcdef")),
            ] {
                store
                    .catalog
                    .set(&store.node_id, Key::project(&id, field), value)
                    .unwrap();
            }
        }
        let current = snapshot(&store, &query).unwrap();
        let first = page(&query, &store, &current).unwrap();
        assert_eq!(first["projects"].as_array().unwrap().len(), 100);
        let mut next = query.clone();
        next.cursor = first["next_cursor"].as_str().map(str::to_owned);
        let last = page(&next, &store, &current).unwrap();
        assert_eq!(last["projects"].as_array().unwrap().len(), 8);
        assert!(last["next_cursor"].is_null());
        assert!(current
            .projects
            .iter()
            .any(|p| p.name == "Два ' $(literal)"));
        store
            .catalog
            .set(&store.node_id, Key::project("two", "alive"), json!(false))
            .unwrap();
        assert!(page(&next, &store, &snapshot(&store, &query).unwrap()).is_err());
    }
    #[test]
    fn durable_duplicate_is_payload_bound_and_unfinished_never_replays() {
        let (store, query) = fixture();
        let request = mutation(&store, &query);
        let mut receipts = Receipts::new();
        let mut result = request.outcome("assigned");
        result["current_project_id"] = json!("two");
        receipts.insert(request.request_id.clone(),json!({"kind":"session-project-v1","request":request,"phase":"terminal","outcome":result}));
        assert_eq!(recorded(&receipts, &request).unwrap(), Some(result));
        let mut changed = request.clone();
        changed.project_id = Some("one".into());
        assert!(recorded(&receipts, &changed).is_err());
        receipts.get_mut(&request.request_id).unwrap()["phase"] = json!("attempting");
        assert_eq!(
            recorded(&receipts, &request).unwrap().unwrap()["status"],
            "uncertain"
        );
        let before = serde_json::to_value(&store).unwrap();
        for _ in 0..3 {
            assert_eq!(
                recorded(&receipts, &request).unwrap().unwrap()["status"],
                "uncertain"
            );
        }
        assert_eq!(serde_json::to_value(store).unwrap(), before);
        receipts.get_mut(&request.request_id).unwrap()["kind"] = json!("launch");
        assert!(recorded(&receipts, &request).is_err());
    }
    #[test]
    fn actual_commit_path_reserves_before_write_and_lost_outcome_is_never_reapplied() {
        let (store, query) = fixture();
        let request = mutation(&store, &query);
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("receipts.json");
        let mut journal = Receipts::new();
        let calls = std::cell::Cell::new(0);
        let result = commit_assignment(&path, &mut journal, &request, || {
            calls.set(calls.get() + 1);
            let saved = receipts(&path)?;
            assert_eq!(saved[&request.request_id]["phase"], "attempting");
            anyhow::bail!("synthetic catalog persistence failure")
        })
        .unwrap();
        assert_eq!(result["status"], "uncertain");
        assert_eq!(calls.get(), 1);
        let saved = receipts(&path).unwrap();
        assert_eq!(
            recorded(&saved, &request).unwrap().unwrap()["status"],
            "uncertain"
        );
        assert_eq!(calls.get(), 1);
        let mut retry = saved.clone();
        let duplicate = commit_assignment(&path, &mut retry, &request, || {
            calls.set(calls.get() + 1);
            Ok(())
        })
        .unwrap();
        assert_eq!(duplicate["status"], "uncertain");
        assert_eq!(calls.get(), 1);
        let mut changed = request.clone();
        changed.project_id = Some("one".into());
        assert!(commit_assignment(&path, &mut retry, &changed, || {
            calls.set(calls.get() + 1);
            Ok(())
        })
        .is_err());
        assert_eq!(calls.get(), 1);
        let mut next = request.clone();
        next.request_id = new_id();
        let mut journal = saved;
        let result = commit_assignment(&path, &mut journal, &next, || {
            let saved = receipts(&path)?;
            assert_eq!(saved[&next.request_id]["phase"], "attempting");
            std::fs::write(dir.path().join("synthetic-catalog"), b"confirmed grouping")?;
            // Model failure of the final receipt replacement after catalog commit.
            std::fs::remove_file(&path)?;
            std::fs::create_dir(&path)?;
            Ok(())
        })
        .unwrap();
        assert_eq!(result["status"], "uncertain");
        assert_eq!(
            std::fs::read(dir.path().join("synthetic-catalog")).unwrap(),
            b"confirmed grouping"
        );
        assert!(receipts(&path).is_err());
    }

    #[test]
    fn malformed_or_foreign_receipt_files_never_become_an_empty_journal() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("receipts.json");
        assert!(receipts(&path).unwrap().is_empty());
        std::fs::write(&path, b"not-json").unwrap();
        assert!(receipts(&path).is_err());
        std::fs::remove_file(&path).unwrap();
        std::fs::create_dir(&path).unwrap();
        assert!(receipts(&path).is_err());
        #[cfg(unix)]
        {
            std::fs::remove_dir(&path).unwrap();
            std::os::unix::fs::symlink(dir.path().join("missing"), &path).unwrap();
            assert!(receipts(&path).is_err());
        }
    }
}
