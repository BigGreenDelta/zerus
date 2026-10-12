//! Rename mutable labels without changing run identity or restarting the agent.
use super::*;
use std::fs::{self, File};

fn remove(path: &Path) -> Result<()> {
    match fs::remove_file(path) {
        Ok(()) => File::open(path.parent().ok_or("invalid state path")?)
            .and_then(|file| file.sync_all())
            .map_err(|e| e.to_string()),
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(error.to_string()),
    }
}

fn load(path: &Path) -> Result<Value> {
    serde_json::from_str(&fs::read_to_string(path).map_err(|e| e.to_string())?)
        .map_err(|e| format!("{}: {e}", path.display()))
}

fn json_files(directory: &Path) -> Result<Vec<PathBuf>> {
    if !directory.exists() {
        return Ok(Vec::new());
    }
    let mut paths: Vec<_> = fs::read_dir(directory)
        .map_err(|e| e.to_string())?
        .filter_map(|entry| entry.ok().map(|entry| entry.path()))
        .filter(|path| path.extension().and_then(|e| e.to_str()) == Some("json"))
        .collect();
    paths.sort();
    Ok(paths)
}

fn validate(old: &str, new: &str) -> Result<()> {
    let a: Vec<_> = old.split('/').collect();
    let b: Vec<_> = new.split('/').collect();
    let valid = |parts: &[&str]| {
        (2..=3).contains(&parts.len())
            && parts.iter().all(|part| {
                !part.is_empty()
                    && !part.contains([':', '.'])
                    && part.trim() == *part
                    && !part.chars().any(char::is_control)
            })
    };
    if !valid(&a) || !valid(&b) || b.len() != 3 || a[..2] != b[..2] {
        return Err("rename must keep the same agent/project and use a nonempty tag without '/', ':', '.', control characters or surrounding whitespace".into());
    }
    Ok(())
}

fn session_id(name: &str) -> Result<String> {
    let output = tmux(
        &[
            "display-message".into(),
            "-p".into(),
            "-t".into(),
            format!("={name}:"),
            "#{session_id}".into(),
        ],
        true,
    )?;
    let id = String::from_utf8_lossy(&output.stdout).trim().to_owned();
    if !id.starts_with('$') || !id[1..].chars().all(|c| c.is_ascii_digit()) {
        return Err("invalid tmux session identity".into());
    }
    Ok(id)
}

fn current_session_name(id: &str) -> Result<Option<String>> {
    let output = tmux(
        &[
            "display-message".into(),
            "-p".into(),
            "-t".into(),
            id.into(),
            "#{session_name}".into(),
        ],
        false,
    )?;
    Ok(output
        .status
        .success()
        .then(|| String::from_utf8_lossy(&output.stdout).trim().to_owned()))
}

pub(super) fn dispatch(args: &[String], dry: bool) -> Result<i32> { dispatch_scoped(args,dry,None) }

pub(super) fn dispatch_scoped(args: &[String], dry: bool, scope: Option<&session_action::Scope>) -> Result<i32> {
    let archive_id = match args {
        [_, _] => None,
        [_, _, flag, id] if flag == "--archive" => Some(id.as_str()),
        _ => return Err("usage: hgs rename <session> <new-name> [--archive ID]".into()),
    };
    let old = &args[0];
    let new = &args[1];
    validate(old, new)?;
    let guard = lock(None)?;
    let snapshot = live()?;
    if archive_id.is_none()
        && old != new
        && (snapshot.contains_key(new) || record_path(new).exists())
    {
        return Err(format!(
            "{new}: a live or saved session already uses this name"
        ));
    }
    if let Some(id) = archive_id {
        let mut record = archive::read_archive(old, id)?;
        if let Some(scope)=scope {scope.check(&record)?;}
        if old != new && !dry {
            anchor(&mut record);
            record["name"] = json!(new);
            record["renamed_at"] = json!(now());
            for key in ["rename_shadow", "rename_target", "legacy_shadow"] {
                record.as_object_mut().unwrap().remove(key);
            }
            if let Some(scope)=scope {scope.changing();}
            atomic(&archive::path(id)?, &format!("{record}\n"))?;
        }
        if let Some(scope)=scope {scope.result(new,string(&record,"run_id"),string(&record,"conversation_id"),Some(id));}
    } else {
        let record = if record_path(old).exists() {
            Some(read(old)?)
        } else {
            None
        };
        if let Some(scope)=scope {scope.check(record.as_ref().ok_or("Scoped rename requires a tracked binding")?)?;}
        let live_panes = snapshot.get(old);
        if record.is_none() && live_panes.is_none() {
            return Err(format!("{old}: session not found"));
        }
        if let Some(record) = &record {
            if live_panes.is_some() && !matches(record, live_panes) {
                return Err(format!("{old}: live pane does not match its saved binding"));
            }
            if record.get("pausing").is_some() || record["resume_pending"] == true {
                return Err(format!(
                    "{old}: wait for the current pause or restore to finish before renaming"
                ));
            }
        }
        if old != new && !dry {
            let id = if live_panes.is_some() {
                Some(session_id(old)?)
            } else {
                None
            };
            let legacy = record.as_ref().is_some_and(|record| {
                record["run_identity_version"] != 1
                    && record["legacy_shadow"].is_null()
                    && record["supervisor"].is_object()
                    && !attempts::identities_gone(record)
            });
            let transaction = json!({"version":1,"old":old,"new":new,"record":record,
                "session_id":id,"legacy":legacy,
                "source_routed":route_path(old).exists()});
            let path = root()
                .join("renames")
                .join(format!("{}.json", uuid::Uuid::new_v4()));
            if let Some(scope)=scope {scope.changing();}
            atomic(&path, &format!("{transaction}\n"))?;
            if let Some(id) = id {
                if let Err(error) = tmux(
                    &["rename-session".into(), "-t".into(), id, new.into()],
                    true,
                ) {
                    remove(&path)?;
                    return Err(error);
                }
            }
            commit(&transaction)?;
            remove(&path)?;
        }
    }
    let reported = match scope {
        Some(scope) if archive_id.is_none() => read(new).map(|record| scope.result_record(&record)),
        Some(_) => Ok(()),
        None => {
            println!("hgs: {}renamed {old} to {new}", if dry { "would have " } else { "" });
            Ok(())
        }
    };
    // Hooks share the state lock, so Overmind records the rename after it. An
    // archived conversation can share its name with a live session, whose
    // tasks must stay where they are.
    drop(guard);
    if archive_id.is_none() && old != new && !dry {
        crate::overmind::session_renamed(old, new);
    }
    reported.map(|()| 0)
}

fn anchor(record: &mut Value) {
    if string(record, "journal_name").is_empty() {
        record["journal_name"] = record["name"].clone();
    }
}

fn source_path(transaction: &Value) -> PathBuf {
    let legacy = legacy_record_path(string(transaction, "old"));
    if transaction["source_routed"] == true {
        root().join("bindings").join(legacy.file_name().unwrap())
    } else {
        legacy
    }
}

fn commit(transaction: &Value) -> Result<()> {
    let old = string(transaction, "old");
    let new = string(transaction, "new");
    validate(old, new)?;
    let Some(mut record) = transaction["record"]
        .as_object()
        .map(|_| transaction["record"].clone())
    else {
        return Ok(());
    };
    let source = source_path(transaction);
    if source.exists() {
        let current = load(&source)?;
        if current["run_id"] != record["run_id"] {
            return Err("rename source run changed; refusing to replace its binding".into());
        }
        if current["rename_shadow"] != true {
            record = current;
        }
    }
    if record_path(new).exists() {
        let destination = read(new)?;
        if destination["run_id"] != record["run_id"] {
            return Err("rename destination was reused; refusing to replace its binding".into());
        }
    }
    anchor(&mut record);
    record["name"] = json!(new);
    record["renamed_at"] = json!(now());
    if transaction["legacy"] == true {
        record["legacy_shadow"] = json!({"name":old,"run_id":record["run_id"]});
    }
    // Publish the canonical copy before retiring the old binding. Hooks share
    // this lock, and recovery completes an interrupted transaction first.
    recovery::rename_job(old,new,string(&record,"run_id"))?;
    write(&mut record)?;
    if transaction["legacy"] == true {
        atomic(
            &route_path(old),
            &format!(
                "{}\n",
                json!({"version":1,"name":old,"run_id":record["run_id"]})
            ),
        )?;
        let mut shadow = record.clone();
        shadow["name"] = json!(old);
        shadow["rename_target"] = json!(new);
        shadow["rename_shadow"] = json!(true);
        atomic(&legacy_record_path(old), &format!("{shadow}\n"))?;
    } else {
        remove(&source)?;
    }
    // A legacy supervisor can finish between the process crash and recovery.
    // Preserve its archive under the requested final label as well.
    for path in json_files(&root().join("archives"))? {
        let mut archived = load(&path)?;
        if archived["run_id"] == record["run_id"] && string(&archived, "name") == old {
            anchor(&mut archived);
            archived["name"] = json!(new);
            archived.as_object_mut().unwrap().remove("rename_shadow");
            archived.as_object_mut().unwrap().remove("rename_target");
            atomic(&path, &format!("{archived}\n"))?;
        }
    }
    Ok(())
}

/// Called immediately after obtaining the writer lock, never recursively.
pub(super) fn recover_locked() -> Result<()> {
    for path in json_files(&root().join("renames"))? {
        let transaction = load(&path)?;
        if transaction["version"] != 1 {
            return Err("invalid rename transaction".into());
        }
        let old = string(&transaction, "old");
        let new = string(&transaction, "new");
        validate(old, new)?;
        if let Some(id) = transaction["session_id"].as_str() {
            if let Some(current) = current_session_name(id)? {
                if current == old {
                    // tmux rename never happened. All durable bindings still
                    // use the old name, so aborting is lossless.
                    remove(&path)?;
                    continue;
                }
                if current != new {
                    return Err("tmux session changed during rename recovery".into());
                }
            }
        }
        commit(&transaction)?;
        remove(&path)?;
        // Rare, and Overmind waits at most a second, so this may run under the lock.
        crate::overmind::session_renamed(old, new);
    }
    Ok(())
}

pub(super) fn mirror_legacy(record: &Value) -> Result<()> {
    let legacy = &record["legacy_shadow"];
    let old = string(legacy, "name");
    if old.is_empty() || legacy["run_id"] != record["run_id"] {
        return Ok(());
    }
    let path = legacy_record_path(old);
    if !path.exists() {
        return Ok(());
    }
    let current = load(&path)?;
    if current["run_id"] != record["run_id"] {
        return Err("legacy rename shadow has a different run identity".into());
    }
    let mut shadow = record.clone();
    shadow["name"] = json!(old);
    shadow["rename_target"] = record["name"].clone();
    shadow["rename_shadow"] = json!(true);
    atomic(&path, &format!("{shadow}\n"))
}

pub(super) fn sync_legacy_locked() -> Result<()> {
    for route in json_files(&root().join("name_routes"))? {
        let route = load(&route)?;
        let old = string(&route, "name");
        let path = legacy_record_path(old);
        if !path.exists() {
            continue;
        }
        let shadow = load(&path)?;
        if shadow["rename_shadow"] != true || shadow["run_id"] != route["run_id"] {
            return Err("invalid legacy rename shadow identity".into());
        }
        let name = string(&shadow, "rename_target");
        if name.is_empty() || !record_path(name).exists() {
            continue;
        }
        let mut record = read(name)?;
        if record["run_id"] != shadow["run_id"] {
            continue;
        }
        if record["supervisor"] != shadow["supervisor"] {
            return Err("legacy supervisor completion identity changed".into());
        }
        let completed = shadow["exited_at"].as_f64().unwrap_or(0.0)
            > record["exited_at"].as_f64().unwrap_or(0.0);
        let spawned = shadow["updated"].as_f64().unwrap_or(0.0)
            > record["updated"].as_f64().unwrap_or(0.0)
            && (record["pid"] != shadow["pid"]
                || record["process_start"] != shadow["process_start"]);
        if completed || spawned {
            // A pre-upgrade runner may move from a failed initial resume to
            // its fresh fallback after rename. Only that immutable supervisor
            // and run can publish the next tracked child's PID/start pair.
            if shadow["pid"].as_u64().unwrap_or(0) == 0 {
                return Err("legacy supervisor published an invalid process identity".into());
            }
            record["pid"] = shadow["pid"].clone();
            record["process_start"] = shadow["process_start"].clone();
            if completed {
                for key in [
                    "exit_code",
                    "termination_signal",
                    "exited_at",
                    "completion_source",
                    "completion_reason",
                ] {
                    if let Some(value) = shadow.get(key) {
                        record[key] = value.clone();
                    }
                }
            }
            write(&mut record)?;
        }
    }
    Ok(())
}

pub(super) fn normalize_archive(record: &mut Value) -> Result<()> {
    if record["rename_shadow"] == true && !string(record, "rename_target").is_empty() {
        validate(string(record, "name"), string(record, "rename_target"))?;
        if record["legacy_shadow"]["run_id"] != record["run_id"]
            || record["legacy_shadow"]["name"] != record["name"]
        {
            return Err("invalid archived rename shadow identity".into());
        }
        record["name"] = record["rename_target"].clone();
        record.as_object_mut().unwrap().remove("rename_shadow");
        record.as_object_mut().unwrap().remove("rename_target");
    }
    Ok(())
}

pub(super) fn remove_legacy(record: &Value) -> Result<()> {
    let legacy = &record["legacy_shadow"];
    let old = string(legacy, "name");
    if old.is_empty() || legacy["run_id"] != record["run_id"] {
        return Ok(());
    }
    let path = legacy_record_path(old);
    if path.exists() && load(&path)?["run_id"] == record["run_id"] {
        remove(&path)?;
    }
    Ok(())
}
