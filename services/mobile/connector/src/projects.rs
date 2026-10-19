//! The native logical-project view (`projects.py`) for heartbeats and the
//! launch catalog, without peer settings or CRDT history.
use crate::pyvalue::{hashable, is_true, py_eq, py_iter, py_strip, PyError, PyResult};
use serde_json::{json, Map, Value};
use std::collections::{HashMap, HashSet};
use zerus_relay::json::digest;

/// `projects.is_archived(row)`.
pub fn is_archived(row: &Map<String, Value>) -> bool {
    row.get("state").and_then(Value::as_str) == Some("archived")
        || row
            .get("archive_id")
            .and_then(Value::as_str)
            .is_some_and(|id| !py_strip(id).is_empty())
}

fn invalid(message: &str) -> PyError {
    PyError::value(message)
}

/// `projects.normalize(catalog, snapshot, fallback_id)`.
pub fn normalize(catalog: &Value, snapshot: &Value, fallback_id: &str) -> PyResult<Value> {
    let catalog = match catalog.as_object() {
        Some(catalog)
            if catalog
                .get("schema")
                .is_some_and(|schema| py_eq(schema, &json!(1)))
                && catalog.get("organization").is_some_and(Value::is_object) =>
        {
            catalog
        }
        _ => return Err(invalid("invalid native project catalog")),
    };
    let initialized = is_true(catalog.get("initialized"));
    let organization = catalog["organization"].as_object().expect("checked");
    let groups = match organization.get("projects") {
        Some(Value::Array(groups)) if groups.len() <= 5000 => groups,
        _ => return Err(invalid("invalid native logical projects")),
    };
    if !initialized {
        return Ok(unavailable(fallback_id));
    }
    let text = |key: &str| {
        catalog
            .get(key)
            .and_then(Value::as_str)
            .filter(|value| !value.is_empty())
            .map(str::to_owned)
    };
    let (Some(node_id), Some(swarm_id)) = (text("node_id"), text("swarm_id")) else {
        return Err(invalid("invalid native project identity"));
    };
    let mut aliases: HashSet<String> = HashSet::new();
    if let Some(host) = snapshot.get("host").and_then(Value::as_str) {
        aliases.insert(host.into());
    }
    for machine in py_iter(catalog.get("machines").unwrap_or(&json!([])))? {
        if let Some(machine) = machine.as_object() {
            if machine.get("id").and_then(Value::as_str) == Some(node_id.as_str()) {
                if let Some(connection) = machine.get("connection").and_then(Value::as_str) {
                    aliases.insert(connection.into());
                }
            }
        }
    }
    let mut projects = Vec::new();
    let mut ids: HashSet<String> = HashSet::new();
    for group in groups {
        let group = match group.as_object() {
            Some(group) => group,
            None => return Err(invalid("invalid native project group")),
        };
        let id = group.get("id").and_then(Value::as_str).unwrap_or_default();
        let name = group
            .get("name")
            .and_then(Value::as_str)
            .unwrap_or_default();
        if id.is_empty() || ids.contains(id) || name.is_empty() {
            return Err(invalid("invalid native project group"));
        }
        ids.insert(id.into());
        let mut folders = Vec::new();
        for folder in py_iter(group.get("folders").unwrap_or(&json!([])))? {
            let Some(folder) = folder.as_object() else {
                continue;
            };
            if folder.get("machine_id").and_then(Value::as_str) != Some(node_id.as_str()) {
                continue;
            }
            let mut row = Map::new();
            for key in ["id", "name", "path", "machine_id", "machine_name"] {
                row.insert(key.into(), folder.get(key).cloned().unwrap_or(json!("")));
            }
            row.insert("local".into(), json!(true));
            folders.push(Value::Object(row));
        }
        let mut sessions = Vec::new();
        let mut archives = Vec::new();
        for membership in py_iter(group.get("sessions").unwrap_or(&json!([])))? {
            let Some(membership) = membership.as_str().filter(|text| text.contains('\n')) else {
                continue;
            };
            let (machine, session) = membership.split_once('\n').expect("checked");
            if !aliases.contains(machine) {
                continue;
            }
            if let Some(archive) = session.strip_prefix("archive\n") {
                archives.push(json!(archive));
            } else if !session.is_empty() {
                sessions.push(json!(session));
            }
        }
        projects.push(json!({
            "id": id, "name": name,
            "color": group.get("color").cloned().unwrap_or(json!("#64b5f6")),
            "accessible": group.get("accessible").cloned().unwrap_or(json!(true)),
            "folders": folders, "sessions": sessions, "archives": archives,
        }));
    }
    // `default if default in ids else None`; a list or dict is unhashable.
    let default = match organization.get("default_project") {
        Some(value) => match hashable(value)? {
            Value::String(text) if ids.contains(text) => json!(text),
            _ => Value::Null,
        },
        None => Value::Null,
    };
    Ok(
        json!({"schema": 1, "source": "swarm", "available": true, "stale": false,
              "swarm_id": swarm_id, "node_id": node_id,
              "default_project": default, "projects": projects}),
    )
}

/// Each project's tasks digest from `hgs task digests --json`
/// (docs/superpowers/specs/2026-10-18-task-replication-loro-design.md, "Phone"):
/// a phone exchanges with this computer when its own digest differs. A
/// project without a valid digest carries none.
pub fn apply_task_digests(catalog: &mut Value, digests: &Value) {
    if digests.get("schema") != Some(&json!(1)) {
        return;
    }
    let Some(Value::Array(projects)) = catalog.get_mut("projects") else {
        return;
    };
    for project in projects.iter_mut().filter_map(Value::as_object_mut) {
        let id = project
            .get("id")
            .and_then(Value::as_str)
            .unwrap_or_default();
        let digest = digests["projects"][id]["digest"].as_str().filter(|digest| {
            digest.len() == 64
                && digest
                    .bytes()
                    .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
        });
        if let Some(digest) = digest {
            project.insert("tasks_digest".into(), json!(digest));
        }
    }
}

/// `projects.unavailable(fallback_id)`.
pub fn unavailable(fallback_id: &str) -> Value {
    json!({"schema": 1, "source": "swarm", "available": false, "stale": false,
           "swarm_id": format!("local:{}", digest(fallback_id)),
           "node_id": null, "default_project": null, "projects": []})
}

/// `projects.apply_memberships(snapshot, catalog)`: each session row gets
/// the project that lists it, else the default project. A key Python could
/// not hash finds nothing here, where Python's snapshot raised a TypeError
/// that stopped its heartbeat task.
pub fn apply_memberships(sessions: &mut [Value], catalog: &Value) {
    if !crate::pyvalue::truthy(catalog.get("available").unwrap_or(&Value::Null)) {
        return;
    }
    let mut by_session: HashMap<String, Value> = HashMap::new();
    let mut by_archive: HashMap<String, Value> = HashMap::new();
    for project in catalog["projects"].as_array().into_iter().flatten() {
        for session in project["sessions"].as_array().into_iter().flatten() {
            if let Some(session) = session.as_str() {
                by_session
                    .entry(session.into())
                    .or_insert_with(|| project["id"].clone());
            }
        }
        for archive in project["archives"].as_array().into_iter().flatten() {
            if let Some(archive) = archive.as_str() {
                by_archive
                    .entry(archive.into())
                    .or_insert_with(|| project["id"].clone());
            }
        }
    }
    let default = catalog
        .get("default_project")
        .cloned()
        .unwrap_or(Value::Null);
    for session in sessions.iter_mut() {
        let Some(row) = session.as_object_mut() else {
            continue;
        };
        let group = if is_archived(row) {
            row.get("archive_id")
                .and_then(Value::as_str)
                .and_then(|id| by_archive.get(id))
        } else {
            row.get("name")
                .and_then(Value::as_str)
                .and_then(|name| by_session.get(name))
        };
        let group = group.cloned().unwrap_or_else(|| default.clone());
        if !group.is_null() {
            row.insert("mobile_project_id".into(), group);
        }
    }
}

/// `launch.projects(raw)`: the accessible projects for a launch catalog.
pub fn launch_projects(raw: &Value) -> PyResult<Value> {
    let value = normalize(raw, &json!({"sessions": []}), "launch")?;
    let projects = value["projects"].as_array().expect("normalized");
    if value["available"] != json!(true) || projects.len() > 500 {
        return Err(invalid("native project catalog is unavailable"));
    }
    let accessible: Vec<Value> = projects
        .iter()
        .filter(|row| crate::pyvalue::truthy(&row["accessible"]))
        .map(|row| {
            let mut kept = Map::new();
            for key in ["id", "name", "color", "folders", "accessible"] {
                kept.insert(key.into(), row[key].clone());
            }
            Value::Object(kept)
        })
        .collect();
    Ok(
        json!({"project_launch_supported": true, "swarm_id": value["swarm_id"],
              "default_project": value["default_project"], "projects": accessible}),
    )
}
