//! Immutable completion records, indexed independently of reusable tmux names.
use super::*;
use std::fs;

pub(super) fn path(id: &str) -> Result<PathBuf> {
    uuid::Uuid::parse_str(id).map_err(|_| "invalid archive ID")?;
    Ok(root().join("archives").join(format!("{id}.json")))
}

pub(super) fn read_archive(name: &str, id: &str) -> Result<Value> {
    let mut record: Value = serde_json::from_str(
        &fs::read_to_string(path(id)?)
            .map_err(|error| format!("{name}: cannot read archive {id}: {error}"))?,
    )
    .map_err(|error| error.to_string())?;
    rename::normalize_archive(&mut record)?;
    if !record.is_object()
        || record["version"] != 1
        || string(&record, "name") != name
        || string(&record, "archive_id") != id
        || !AGENTS.contains(&string(&record, "agent"))
    {
        return Err(format!("{name}: invalid archive record {id}"));
    }
    Ok(record)
}

pub(super) fn records() -> Result<Vec<Value>> {
    let directory = root().join("archives");
    if !directory.exists() {
        return Ok(Vec::new());
    }
    let mut paths: Vec<_> = fs::read_dir(directory)
        .map_err(|e| e.to_string())?
        .filter_map(|entry| entry.ok().map(|entry| entry.path()))
        .filter(|path| path.extension().and_then(|extension| extension.to_str()) == Some("json"))
        .collect();
    paths.sort();
    let mut result = Vec::new();
    for path in paths {
        let loaded = (|| {
            let mut value: Value =
                serde_json::from_str(&fs::read_to_string(&path).map_err(|e| e.to_string())?)
                    .map_err(|e| e.to_string())?;
            rename::normalize_archive(&mut value)?;
            read_archive(
                string(&value, "name"),
                path.file_stem().and_then(|id| id.to_str()).unwrap_or(""),
            )
        })();
        match loaded {
            Ok(record) => result.push(record),
            Err(error) => eprintln!("hgs: cannot read {}: {error}", path.display()),
        }
    }
    result.sort_by(|a, b| {
        b["archived_at"]
            .as_f64()
            .unwrap_or_default()
            .total_cmp(&a["archived_at"].as_f64().unwrap_or_default())
    });
    Ok(result)
}

pub(super) fn remove_synced(path: &Path) -> Result<()> {
    match fs::remove_file(path) {
        Ok(()) => crate::platform::sync_dir(path.parent().ok_or("invalid record path")?)
            .map_err(|error| error.to_string()),
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(error.to_string()),
    }
}

/// The caller holds the state writer lock. Write the durable copy before
/// releasing its old name; a crash between the two operations cannot lose it.
pub(super) fn save(record: &Value) -> Result<String> {
    let db=archive_catalog::catalog()?;
    let existing=archive_catalog::find(&db,record)?;
    let id = if let Some(existing) = existing {
        existing
    } else {
        let id = uuid::Uuid::new_v4().to_string();
        let mut archived = record.clone();
        archived["archive_id"] = json!(id);
        archived["archived_at"] = json!(now());
        archived["state"] = json!("archived");
        archived.as_object_mut().unwrap().remove("pausing");
        atomic(&path(&id)?, &format!("{archived}\n"))?;
        id
    };
    if record_path(string(record, "name")).exists() {
        let current = read(string(record, "name"))?;
        if current["run_id"] == record["run_id"]
            && current["conversation_id"] == record["conversation_id"]
        {
            remove_synced(&record_path(string(record, "name")))?;
            rename::remove_legacy(&current)?;
        }
    }
    Ok(id)
}

pub(super) fn equivalent(record: &Value, archived: &[Value]) -> bool {
    archived.iter().any(|entry| {
        entry["name"] == record["name"]
            && entry["run_id"] == record["run_id"]
            && entry["conversation_id"] == record["conversation_id"]
    })
}

/// Legacy Claude already provides an unambiguous exit reason. Reconcile only
/// after its pane and exact process have gone; ambiguous old records stay saved.
pub(super) fn reconcile() -> Result<()> {
    if !root().exists() {
        return Ok(());
    }
    let _guard = lock(None)?;
    reconcile_locked(&live()?).map(drop)
}

/// Indexed membership for one current binding. The archive JSON stays
/// authoritative: catalog() refreshes a changed archive directory before the
/// lookup. This can rebuild that index, but never enumerates saved bindings.
pub(super) fn contains_selected(record: &Value) -> Result<bool> {
    if !root().join("archives").exists() {
        return Ok(false);
    }
    archive_catalog::contains(&archive_catalog::catalog()?, record)
}

fn reconcile_candidate(record: &Value, snapshot: &Snapshot) -> bool {
    !snapshot.contains_key(string(record, "name"))
        && !process_alive(record)
        && record.get("pausing").is_none()
        && !record["paused"].as_bool().unwrap_or(false)
        && string(record, "expected_id").is_empty()
}

fn clean_completion(record: &Value) -> bool {
    record["termination_signal"].is_null()
        && known_exit(record)
        && ((string(record, "agent") == "claude"
            && record.get("supervisor").is_none()
            && string(record, "error").is_empty())
            || (string(record, "completion_source") == "supervisor"
                && string(record, "completion_reason") == "clean_exit"
                && record["exit_code"] == 0))
}

/// Read-mark admission reconciles just its requested binding. Global lifecycle
/// reconciliation remains in listings; an acknowledgement must not sweep every
/// unrelated saved session while holding the shared-read writer.
pub(super) fn reconcile_selected(name: &str) -> Result<()> {
    if !root().exists() {
        return Ok(());
    }
    let _guard = lock(None)?;
    if !record_path(name).exists() {
        return Ok(());
    }
    let record = read(name)?;
    if reconcile_candidate(&record, &live()?)
        && (contains_selected(&record)? || clean_completion(&record))
    {
        save(&record)?;
    }
    Ok(())
}

/// `reconcile` for a caller that holds the state lock and read `snapshot`
/// under it. Returns the archives and the bindings as it leaves them.
pub(super) fn reconcile_locked(snapshot: &Snapshot) -> Result<(Vec<Value>, Vec<Value>)> {
    let archived = records()?;
    let bindings = super::storage::records()?;
    let mut saved = false;
    for record in &bindings {
        if snapshot.contains_key(string(record, "name"))
            || process_alive(record)
            || record.get("pausing").is_some()
            || record["paused"].as_bool().unwrap_or(false)
            || !string(record, "expected_id").is_empty()
        {
            continue;
        }
        if equivalent(record, &archived) {
            save(record)?;
            saved = true;
            continue;
        }
        let explicit_claude = string(record, "agent") == "claude"
            && record.get("supervisor").is_none()
            && record["termination_signal"].is_null()
            && string(record, "error").is_empty()
            && known_exit(record);
        let confirmed_supervisor = string(record, "completion_source") == "supervisor"
            && string(record, "completion_reason") == "clean_exit"
            && record["exit_code"] == 0
            && record["termination_signal"].is_null()
            && known_exit(record);
        if explicit_claude || confirmed_supervisor {
            save(record)?;
            saved = true;
        }
    }
    if saved {
        return Ok((records()?, super::storage::records()?));
    }
    Ok((archived, bindings))
}

/// Active polling never opens every archived record or its conversation journal.
/// Only bindings that might need reconciliation consult the metadata index.
pub(super) fn reconcile_active_locked(snapshot: &Snapshot) -> Result<Vec<Value>> {
    let bindings = super::storage::records()?;
    let mut result = Vec::new();
    let mut catalog=None;
    for record in bindings {
        // A live exact binding cannot be the crash-left copy of a completed run.
        if matches(&record,snapshot.get(string(&record,"name"))) || process_alive(&record) { result.push(record);continue; }
        if catalog.is_none(){catalog=Some(archive_catalog::catalog()?);}
        if archive_catalog::contains(catalog.as_ref().unwrap(), &record)? { save(&record)?; continue; }
        if snapshot.contains_key(string(&record, "name")) || process_alive(&record)
            || record.get("pausing").is_some() || record["paused"].as_bool().unwrap_or(false)
            || !string(&record, "expected_id").is_empty() {
            result.push(record); continue;
        }
        let clean = record["termination_signal"].is_null() && known_exit(&record) &&
            ((string(&record,"agent")=="claude" && record.get("supervisor").is_none() && string(&record,"error").is_empty()) ||
             (string(&record,"completion_source")=="supervisor" && string(&record,"completion_reason")=="clean_exit" && record["exit_code"]==0));
        if clean { save(&record)?; } else { result.push(record); }
    }
    Ok(result)
}

pub(super) fn manual(name: &str, dry: bool) -> Result<i32> { manual_scoped(name,dry,None) }

pub(super) fn manual_scoped(name: &str, dry: bool, scope: Option<&session_action::Scope>) -> Result<i32> {
    let _guard = lock(None)?;
    if live()?.contains_key(name) {
        return Err(format!(
            "{name}: only stopped or paused sessions can be archived"
        ));
    }
    let mut record = read(name)?;
    if let Some(scope)=scope {scope.check(&record)?;}
    if process_alive(&record) {
        return Err(format!("{name}: tracked agent process is still running"));
    }
    if string(&record, "conversation_id").is_empty() {
        return Err(format!("{name}: conversation ID is not confirmed"));
    }
    if dry {
        println!("hgs: would archive {name}");
        return Ok(0);
    }
    record["completion_source"] = json!("manual_archive");
    record["completion_reason"] = json!("user_archived");
    if let Some(scope)=scope {scope.changing();}
    let id = save(&record)?;
    drop(_guard);
    super::worktree_lifecycle::after_archive(&record);
    if let Some(scope)=scope {scope.result(name,string(&record,"run_id"),string(&record,"conversation_id"),Some(&id));}
    else {println!("hgs: archived {name} ({id})");}
    Ok(0)
}

pub(super) fn forget(name: &str, id: &str, dry: bool) -> Result<()> { forget_scoped(name,id,dry,None) }

pub(super) fn forget_scoped(name: &str, id: &str, dry: bool, scope: Option<&session_action::Scope>) -> Result<()> {
    let _guard = lock(None)?;
    let record=read_archive(name, id)?;
    if let Some(scope)=scope {scope.check(&record)?;}
    // Do not remove the only recoverable source of a not-yet-confirmed restore.
    if record_path(name).exists() && string(&read(name)?, "restore_archive_id") == id {
        return Err(format!(
            "{name}: archive restore is still awaiting confirmation"
        ));
    }
    if dry {
        println!("hgs: would forget archive {id} ({name})");
        return Ok(());
    }
    if let Some(scope)=scope {scope.changing();}
    remove_synced(&path(id)?)
}

/// Commit the restore only after the provider confirms the exact archived ID.
/// The active binding has already been synced by the caller before this runs.
pub(super) fn confirm_restore(record: &mut Value) -> Result<()> {
    let id = string(record, "restore_archive_id").to_owned();
    if id.is_empty() || !string(record, "expected_id").is_empty() {
        return Ok(());
    }
    if path(&id)?.exists() {
        let archived = read_archive(string(record, "name"), &id)?;
        if archived["conversation_id"] != record["conversation_id"] {
            return Err("archive restore confirmed a different conversation".into());
        }
        remove_synced(&path(&id)?)?;
    }
    record.as_object_mut().unwrap().remove("restore_archive_id");
    record.as_object_mut().unwrap().remove("resume_pending");
    write(record)
}

pub(super) fn failed_restore(record: &Value) -> Result<bool> {
    let id = string(record, "restore_archive_id");
    if id.is_empty() || !path(id)?.exists() {
        return Ok(false);
    }
    let mut archived = read_archive(string(record, "name"), id)?;
    archived["last_restore_error"] = json!(if string(record, "error").is_empty() {
        "agent exited before confirming the archived conversation"
    } else {
        string(record, "error")
    });
    atomic(&path(id)?, &format!("{archived}\n"))?;
    remove_synced(&record_path(string(record, "name")))?;
    Ok(true)
}

pub(super) fn known_exit(record: &Value) -> bool {
    let end = &record["session_end"];
    if end["run_id"] != record["run_id"]
        || end["conversation_id"] != record["conversation_id"]
        || string(record, "conversation_state") != "ended"
    {
        return false;
    }
    matches!(
        (string(record, "agent"), string(end, "reason")),
        ("claude", "prompt_input_exit") | ("codex", "other") | ("kimi", "exit")
    )
}
