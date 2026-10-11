use super::*;
use std::fs;

const COMMON: &[&str] = &[
    "SessionStart",
    "SessionEnd",
    "UserPromptSubmit",
    "PreToolUse",
    "PostToolUse",
    "PermissionRequest",
    "SubagentStart",
    "SubagentStop",
    "PreCompact",
    "PostCompact",
    "Stop",
];

fn events(agent: &str) -> Vec<&'static str> {
    let mut events = COMMON.to_vec();
    match agent {
        "claude" => events.extend(["PostToolUseFailure", "StopFailure", "Notification"]),
        "codex" => events.push("Interrupt"),
        "kimi" => events.extend([
            "PostToolUseFailure",
            "StopFailure",
            "Notification",
            "Interrupt",
            "TurnStarted",
            "UserPromptQueued",
            "PermissionResult",
            "TaskStarted",
            "SessionHeartbeat",
        ]),
        _ => {}
    }
    events
}

fn owns_hook(command: &str) -> bool {
    let Ok(argv) = shell_words::split(command) else {
        return false;
    };
    if argv.len() < 3 || argv.last().map(String::as_str) != Some("hook") {
        return false;
    }
    if Path::new(&argv[argv.len() - 2])
        .file_name()
        .and_then(|name| name.to_str())
        == Some("hgs_state.py")
    {
        return true;
    }
    argv[argv.len() - 2] == "__state"
        && Path::new(&argv[argv.len() - 3])
            .file_name()
            .and_then(|name| name.to_str())
            .map(|name| name == "hgs" || name == "hgs.exe")
            .unwrap_or(false)
}

fn resolved_target(path: &Path) -> Result<PathBuf> {
    // Resolve even dangling symlinks, as pathlib.resolve() did, so replacing a
    // dotfile never replaces the user's symlink itself.
    let absolute = if path.is_absolute() {
        path.to_owned()
    } else {
        std::env::current_dir()
            .map_err(|e| e.to_string())?
            .join(path)
    };
    let mut resolved = PathBuf::new();
    let mut remaining: std::collections::VecDeque<std::ffi::OsString> = absolute
        .components()
        .map(|component| component.as_os_str().to_owned())
        .collect();
    let mut links = 0;
    while let Some(component) = remaining.pop_front() {
        if component == "/" {
            resolved.push("/");
            continue;
        }
        if component == "." {
            continue;
        }
        if component == ".." {
            resolved.pop();
            continue;
        }
        resolved.push(&component);
        if let Ok(target) = fs::read_link(&resolved) {
            links += 1;
            if links > 40 {
                return Err(format!("too many symlinks: {}", path.display()));
            }
            resolved.pop();
            if target.is_absolute() {
                resolved.clear();
            }
            let mut pieces: std::collections::VecDeque<_> = target
                .components()
                .map(|c| c.as_os_str().to_owned())
                .collect();
            pieces.append(&mut remaining);
            remaining = pieces;
        }
    }
    Ok(resolved)
}

pub(super) fn install_hooks(agent: &str) -> Result<()> {
    let home_key = home_var(agent)?;
    let directory = nonempty_env(home_key)
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            home().join(if agent == "kimi" {
                ".kimi-code".to_owned()
            } else {
                format!(".{agent}")
            })
        });
    let path = resolved_target(&directory.join(match agent {
        "kimi" => "config.toml",
        "claude" => "settings.json",
        _ => "hooks.json",
    }))?;
    let lock_path = path.with_file_name(format!(
        "{}.hgs.lock",
        path.file_name().unwrap().to_string_lossy()
    ));
    let _guard = lock(Some(&lock_path))?;
    let original = match fs::read_to_string(&path) {
        Ok(contents) => contents,
        Err(error) if error.kind() == io::ErrorKind::NotFound => String::new(),
        Err(error) => return Err(error.to_string()),
    };
    let executable = runner_executable()?;
    let mut command = join_command(&[executable, "__state".into(), "hook".into()]);
    let content;
    if agent == "kimi" {
        let begin = "# BEGIN hgs session tracking\n";
        let end = "# END hgs session tracking\n";
        let mut content_without = original.clone();
        if let Some(start) = original.find(begin) {
            let rest = &original[start + begin.len()..];
            let finish = rest
                .find(end)
                .ok_or_else(|| format!("incomplete hgs hook block in {}", path.display()))?;
            let block = &rest[..finish];
            let document = block
                .parse::<toml_edit::DocumentMut>()
                .map_err(|e| format!("cannot merge tracking hooks into {}: {e}", path.display()))?;
            if let Some(tables) = document
                .get("hooks")
                .and_then(toml_edit::Item::as_array_of_tables)
            {
                if let Some(existing) = tables
                    .iter()
                    .filter_map(|table| table.get("command").and_then(toml_edit::Item::as_str))
                    .find(|value| owns_hook(value))
                {
                    command = existing.to_owned();
                }
                if events(agent).iter().all(|event| {
                    tables.iter().any(|table| {
                        table.get("event").and_then(toml_edit::Item::as_str) == Some(event)
                            && table
                                .get("command")
                                .and_then(toml_edit::Item::as_str)
                                .map(owns_hook)
                                .unwrap_or(false)
                    })
                }) {
                    // Validate the complete document but retain exact trusted
                    // hook bytes, comments, ordering and user settings.
                    original
                        .parse::<toml_edit::DocumentMut>()
                        .map_err(|e| e.to_string())?;
                    return Ok(());
                }
            }
            content_without = format!("{}{}", &original[..start], &rest[finish + end.len()..]);
        }
        let mut block = begin.to_owned();
        for event in events(agent) {
            block.push_str(&format!(
                "[[hooks]]\nevent = {}\ncommand = {}\ntimeout = 5\n",
                json!(event),
                json!(command)
            ));
        }
        block.push_str(end);
        content = format!("{}\n\n{block}", content_without.trim_end_matches('\n'));
        content
            .parse::<toml_edit::DocumentMut>()
            .map_err(|e| format!("cannot merge tracking hooks into {}: {e}", path.display()))?;
    } else {
        let mut document: Value = if original.trim().is_empty() {
            json!({})
        } else {
            serde_json::from_str(&original)
                .map_err(|e| format!("cannot read {}: {e}", path.display()))?
        };
        let object = document
            .as_object_mut()
            .ok_or("hook config must be an object")?;
        let hooks = object
            .entry("hooks")
            .or_insert_with(|| json!({}))
            .as_object_mut()
            .ok_or("hooks must be an object")?;
        if let Some(existing) = hooks
            .values()
            .filter_map(Value::as_array)
            .flatten()
            .filter_map(|group| group.get("hooks").and_then(Value::as_array))
            .flatten()
            .filter_map(|entry| entry.get("command").and_then(Value::as_str))
            .find(|value| owns_hook(value))
        {
            command = existing.to_owned();
        }
        let mut changed = false;
        for event in events(agent) {
            let entries = hooks
                .entry(event)
                .or_insert_with(|| json!([]))
                .as_array_mut()
                .ok_or("hook event must be an array")?;
            let installed = entries
                .iter()
                .filter_map(|group| group.get("hooks").and_then(Value::as_array))
                .flatten()
                .any(|entry| owns_hook(string(entry, "command")));
            if !installed {
                entries.push(
                    json!({"hooks": [{"type": "command", "command": command, "timeout": 5}]}),
                );
                changed = true;
            }
        }
        if !changed {
            return Ok(());
        }
        content = format!(
            "{}\n",
            serde_json::to_string_pretty(&document).map_err(|e| e.to_string())?
        );
    }
    if content != original {
        let backup = path.with_file_name(format!(
            "{}.pre-hgs",
            path.file_name().unwrap().to_string_lossy()
        ));
        if !original.is_empty() && !backup.exists() {
            atomic(&backup, &original)?;
        }
        atomic(&path, &content)?;
    }
    Ok(())
}

pub(super) fn hook() -> Result<i32> {
    let (Some(name), Some(token)) = (nonempty_env("HGS_SESSION"), nonempty_env("HGS_RUN_ID"))
    else {
        return Ok(0);
    };
    let event = stdin_json()?;
    let sid = event
        .get("session_id")
        .and_then(Value::as_str)
        .filter(|sid| !sid.is_empty() && !sid.chars().any(char::is_whitespace))
        .ok_or("hook did not supply a valid session_id")?;
    let kind = string(&event, "hook_event_name");
    let _guard = lock(None)?;
    let Some(mut record) = read_run(&name, &token)? else {
        return Ok(0);
    };
    if !matches(&record, live()?.get(string(&record, "name"))) {
        return Ok(0);
    }
    if let Some(scoped) = external_agents::route(&mut record, &event) {
        processes::observe(&mut record, &scoped);
        journal::update_activity(&mut record, &scoped);
        if kind == "PermissionRequest" {
            let child = &mut record["subagents"][string(&scoped, "agent_id")];
            child["display_state"] = json!("approval");
            // Each request has its own identity, so acknowledging one never hides the next.
            let tool = ["tool_use_id", "tool_call_id"]
                .iter()
                .map(|key| string(&scoped, key))
                .find(|id| !id.is_empty());
            child["attention_id"] =
                json!(tool.map_or_else(|| format!("request:{}", child["updated"]), str::to_owned));
        }
        journal::log_event(&record, &scoped)?;
        write(&mut record)?;
        return Ok(0);
    }
    if kind == "SessionStart" && string(&event, "agent_id").is_empty()
        && !external_agents::main_start(&record, &event) {
        return Ok(0);
    }
    if !events(string(&record, "agent")).contains(&kind) {
        return Ok(0);
    }
    if pause_active(&record) && ["UserPromptSubmit", "PreToolUse", "TurnStarted"].contains(&kind) {
        eprintln!("hgs is pausing this session; retry after resuming");
        return Ok(2);
    }
    let changed_conversation = kind == "SessionStart" && string(&event, "agent_id").is_empty()
        && sid != string(&record, "conversation_id");
    if kind == "SessionStart" && string(&event, "agent_id").is_empty() {
        if !string(&record, "fork_parent_id").is_empty() && sid == string(&record, "fork_parent_id")
        {
            record["error"] =
                json!("provider opened the source conversation instead of a new fork");
            record["activity"] = json!("unknown");
            record["phase"] = json!("unknown");
            write(&mut record)?;
            return Ok(0);
        }
        if sid == string(&record["fresh_from"], "conversation_id") && !sid.is_empty() {
            record["error"] =
                json!("provider opened the previous conversation instead of a fresh one");
            record["activity"] = json!("unknown");
            record["phase"] = json!("unknown");
            write(&mut record)?;
            return Ok(0);
        }
        if !string(&record, "expected_id").is_empty() && sid != string(&record, "expected_id") {
            record["error"] = json!("agent resumed a different conversation");
            record["activity"] = json!("unknown");
            write(&mut record)?;
            return Ok(0);
        }
        if sid != string(&record, "conversation_id") {
            clear_context::observe_start(&mut record, &event);
            for key in [
                "clear_context_request",
                "compact_context_request",
                "prompt",
                "last_message",
                "subagents",
                "active_tools",
                "last_error",
                "subagent_groups",
                "subagent_groups_complete",
                "subagents_completed_pruned",
                "task_lists",
                "pending_task_updates",
                "shell_jobs",
                "pending_settings",
                "resume_settings",
                "effort_source_path",
                "effort_source_offset",
            ] {
                record.as_object_mut().unwrap().remove(key);
            }
        }
        record.as_object_mut().unwrap().remove("expected_id");
        record.as_object_mut().unwrap().remove("session_end");
        record
            .as_object_mut()
            .unwrap()
            .remove("pending_task_updates");
        record["conversation_id"] = json!(sid);
        record["conversation_state"] = json!("active");
        record["activity"] = json!("idle");
        record["phase"] = json!("idle");
        record["main_done"] = json!(true);
        if !string(&event, "cwd").is_empty() {
            record["cwd"] = event["cwd"].clone();
        }
        record["transcript"] = event["transcript_path"].clone();
    } else if sid != string(&record, "conversation_id") {
        return Ok(0);
    }
    if kind == "SessionEnd" && string(&event, "agent_id").is_empty() {
        record["session_end"] = json!({"reason": string(&event, "reason"), "at": now(),
            "run_id": record["run_id"], "conversation_id": sid});
    }
    // Claude's suggested next message is no subagent and no Activity event.
    if prompt_suggestion::observe(&mut record, &event) {
        write(&mut record)?;
        return Ok(0);
    }
    telemetry::observe(&mut record, &event);
    tasks::observe(&mut record, &event);
    processes::observe(&mut record, &event);
    journal::update_activity(&mut record, &event);
    compact_context::observe(&mut record, &event);
    questions::observe(&mut record, &event);
    journal::log_event(&record, &event)?;
    if changed_conversation && record["session_clear"]["journaled"] != true {
        if let Some(mut clear) = clear_context::event(&record) {
            clear["hook_event_name"] = json!("SessionCleared");
            journal::log_event(&record, &clear)?;
        }
    }
    write(&mut record)?;
    if kind == "SessionStart" && string(&event, "agent_id").is_empty() {
        archive::confirm_restore(&mut record)?;
    }
    Ok(0)
}
