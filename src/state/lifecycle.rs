use super::*;
use crate::platform::ExitInfo;
use std::time::{Duration, Instant};

fn required_env(name: &str) -> Result<String> {
    nonempty_env(name).ok_or_else(|| format!("missing {name} in tracked agent launch"))
}

/// Whether a tracked Codex launch passes `--no-daemon`. Older Codex already
/// runs in-process and rejects this newer flag, so the native CLI is asked.
/// A project launcher such as `run_codex.sh` runs only under supervision: it
/// may prompt, unlock secrets or exit before this run is recorded. It receives
/// the flag that reviewed Codex releases require; `--bare` selects the native
/// CLI for an older Codex.
fn codex_daemon_flag(executable: &str) -> Result<bool> {
    if executable != "codex" {
        return Ok(true);
    }
    codex_supports_no_daemon(executable)
}

fn codex_supports_no_daemon(executable: &str) -> Result<bool> {
    use std::process::Stdio;
    // Probe capabilities without starting a conversation or inheriting a hook identity.
    let mut output = tempfile::tempfile().map_err(|e| e.to_string())?;
    let mut command = crate::platform::agent_command(&[executable.into(), "--help".into()])
        .map_err(|error| format!("{executable}: {error}"))?;
    let mut child = crate::platform::spawn(
        command
            .env_remove("HGS_RUN_ID")
            .env_remove("HGS_SESSION")
            .stdin(Stdio::null())
            .stderr(Stdio::null())
            .stdout(output.try_clone().map_err(|e| e.to_string())?),
    )
    .map_err(|e| e.to_string())?;
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        if let Some(status) = child.try_wait().map_err(|e|e.to_string())? {
            if !status.success() { return Err("Cannot check Codex launch capabilities (--help failed)".into()); }
            use std::io::{Read, Seek, SeekFrom};
            output.seek(SeekFrom::Start(0)).map_err(|e|e.to_string())?;
            let mut text=String::new();output.take(128*1024).read_to_string(&mut text).map_err(|e|e.to_string())?;
            return Ok(text.split_whitespace().any(|word|word=="--no-daemon"));
        }
        if Instant::now()>=deadline {
            let _=child.kill();let _=child.wait();return Err("Codex capability check timed out".into());
        }
        std::thread::sleep(Duration::from_millis(20));
    }
}

pub(super) fn run(args: &[String]) -> Result<i32> {
    let first_count: usize = args
        .first()
        .ok_or("missing launch argument count")?
        .parse()
        .map_err(|_| "invalid launch argument count")?;
    if first_count >= args.len() {
        return Err("missing agent command".into());
    }
    let mut first = args[1..=first_count].to_vec();
    let mut base = args[1 + first_count..].to_vec();
    if base.first().map(|arg| arg.is_empty()).unwrap_or(true) {
        return Err("missing agent command".into());
    }
    let mut name = required_env("HGS_SESSION")?;
    let agent = required_env("HGS_AGENT")?;
    home_var(&agent)?;
    let codex_no_daemon = agent == "codex" && codex_daemon_flag(&base[0])?;
    for key in ["CLAUDE_CONFIG_DIR", "CODEX_HOME", "KIMI_CODE_HOME"] {
        if nonempty_env(key).is_none() {
            std::env::remove_var(key);
        }
    }
    if nonempty_env("HGS_ACCOUNT_ID").is_some_and(|id| !id.starts_with("native-")) {
        for key in crate::accounts::AUTH_ENV {
            std::env::remove_var(key);
        }
    }
    if agent == "claude" {
        // The tmux server may predate this SSH connection and still belong to
        // another security session. Join immediately before the native launch.
        let _ = crate::macos_session::join();
        crate::accounts::prepare_claude_settings().map_err(|e|e.message)?;
    }
    crate::accounts::prepare_mcp_servers(&agent).map_err(|e|e.message)?;
    hooks::install_hooks(&agent, &base[0])?;
    let requested = nonempty_env("HGS_REQUESTED_ID");
    let expected = nonempty_env("HGS_EXPECTED_ID");
    let fresh = nonempty_env("HGS_FRESH").as_deref() == Some("1");
    // `resume --fresh` names the exact stopped run it replaces.
    let replaces = nonempty_env("HGS_FRESH_REPLACES");
    std::env::remove_var("HGS_REQUESTED_ID");
    std::env::remove_var("HGS_FRESH");
    std::env::remove_var("HGS_FRESH_REPLACES");
    if requested.is_some() && expected.is_some() {
        return Err("requested and saved resume identities cannot be combined".into());
    }
    if replaces.is_some() && (!fresh || requested.is_some() || expected.is_some()) {
        return Err("a fresh start cannot resume a conversation".into());
    }
    if requested
        .as_ref()
        .is_some_and(|id| id.chars().any(char::is_whitespace))
    {
        return Err("requested conversation ID cannot contain whitespace".into());
    }
    let archive_id = nonempty_env("HGS_ARCHIVE_ID");
    std::env::remove_var("HGS_ARCHIVE_ID");
    let fork_hint_revision = nonempty_env(agent_hint::FORK_REVISION_ENV);
    std::env::remove_var(agent_hint::FORK_REVISION_ENV);
    let _signals = crate::platform::ShutdownGuard::install().map_err(|e| e.to_string())?;
    // The session host's channel for pauses and input checks (spec D10, D11).
    #[cfg(windows)]
    crate::host::supervisor::open();
    let token = nonempty_env("HGS_RUN_ID").unwrap_or_else(|| uuid::Uuid::new_v4().to_string());
    let pane = backend::current().own_pane()?;
    let cwd = std::env::current_dir().map_err(|e| e.to_string())?;
    {
        let _guard = lock(None)?;
        let actual_name = backend::current()
            .pane_info(
                backend::Target::Pane(&pane),
                &[backend::PaneField::SessionName],
            )?
            .value()
            .to_owned();
        if actual_name.is_empty() {
            return Err("launch pane has no session name".into());
        }
        if name != actual_name {
            name = actual_name;
        }
        let mut replaced = None;
        if expected.is_none() && record_path(&name).exists() {
            let previous = read(&name)?;
            if replaces.as_deref().is_some_and(|run| run != string(&previous, "run_id")) {
                return Err(format!("{name}: saved binding changed before a fresh start"));
            }
            attempts::prepare_replacement(&previous, &pane, &token, fresh, requested.as_deref())?;
            replaced = Some(previous);
        } else if replaces.is_some() {
            return Err(format!("{name}: saved binding disappeared before a fresh start"));
        }
        let startup_kind = if expected.is_some() || requested.is_some() || !first.is_empty() {
            "resume"
        } else {
            "new"
        };
        let infer_project = startup_kind == "new" && replaced.is_none()
            && nonempty_env("HGS_FORK_PARENT_ID").is_none();
        let mut record = json!({"version": 1, "run_identity_version": 1, "name": name, "agent": agent, "run_id": token, "pane": pane,
            "cwd": cwd, "launch_dir": cwd, "base": base, "hgs": required_env("HGS_EXECUTABLE")?,
            "shell": nonempty_env("SHELL").unwrap_or_else(|| "/bin/bash".into()), "created": now() as i64,
            "activity": "unknown", "phase": "unknown", "conversation_state": "unknown", "conversation_id": null,
            "transcript": null, "startup_kind":startup_kind,"launch_id":nonempty_env("HGS_LAUNCH_ID"), "account_id": nonempty_env("HGS_ACCOUNT_ID"), "agent_home": nonempty_env(home_var(&agent)?), "pid": std::process::id(),
            "process_start": process_start(std::process::id()),
            "supervisor": {"pid": std::process::id(), "start": process_start(std::process::id())}});
        if let Some(parent) = nonempty_env("HGS_FORK_PARENT_ID") {
            record["fork_parent_id"] = json!(parent);
            record.as_object_mut().unwrap().remove("launch_id");
            record["fork_source_name"] =
                json!(nonempty_env("HGS_FORK_SOURCE_NAME").unwrap_or_default());
        }
        std::env::remove_var("HGS_FORK_PARENT_ID");
        std::env::remove_var("HGS_FORK_SOURCE_NAME");
        agent_hint::inherit_fork(&mut record, fork_hint_revision.as_deref());
        if let Some(expected) = expected {
            let previous = read(&name)?;
            if string(&previous, "conversation_id") != expected
                || string(&previous, "agent") != agent
            {
                return Err(format!("{name}: saved binding changed before agent launch"));
            }
            for key in ["base", "created", "launch_dir"] {
                record[key] = previous
                    .get(key)
                    .ok_or_else(|| format!("{name}: missing saved {key}"))?
                    .clone();
            }
            let restore_id = archive_id
                .clone()
                .or_else(|| nonempty_value(&previous, "restore_archive_id"));
            if let Some(id) = restore_id {
                let archived = archive::read_archive(&name, &id)?;
                if string(&archived, "conversation_id") != expected {
                    return Err("archive resume ID does not match binding".into());
                }
                record["restore_archive_id"] = json!(id);
            }
            record["expected_id"] = json!(expected);
            record["conversation_id"] = json!(expected);
            record["transcript"] = previous["transcript"].clone();
            for key in [
                "prompt",
                "launch_id",
                "last_message",
                "model",
                "effort",
                "effort_source_path",
                "effort_source_offset",
                "resume_settings",
                "pending_settings",
                "fork_parent_id",
                "fork_source_name",
                "cleared_conversations",
            ] {
                if let Some(value) = previous.get(key) {
                    record[key] = value.clone();
                }
            }
            if record["pending_settings"].is_object() {
                record["pending_settings"]["run_id"] = json!(token);
            }
            effort::restore_pending(&mut record)?;
            if let Some(anchor) = previous.get("journal_name") {
                record["journal_name"] = anchor.clone();
            }
            agent_hint::carry_resume(&previous, &mut record);
        } else if let Some(requested) = requested {
            // The ID is an explicit request, not proof that the provider has
            // opened it. Only a matching SessionStart confirms the binding.
            record["expected_id"] = json!(requested);
            record["requested_id"] = json!(requested);
        } else if let Some(previous) = replaced.as_ref().filter(|_| replaces.is_some()) {
            clear_context::carry_fresh_start(previous, &mut record);
        }
        backend::current().set_run_marker(&name, &token)?;
        write(&mut record)?;
        // Keep this exact newly-created binding pinned during optional initial
        // placement. A removal/replacement must not inherit the old folder's
        // inference. The swarm lock is nonblocking and Git shares one 3s cap.
        if infer_project {
            if let Ok(config) = crate::config::Config::load() {
                crate::swarm::infer_new_session(&config, &name, &cwd);
            }
        }
    }
    std::env::set_var("HGS_RUN_ID", &token);
    std::env::set_var("HGS_SESSION", &name);
    std::env::set_var("HGS_STATE_DIR", absolute_root()?);
    std::env::remove_var("HGS_EXPECTED_ID");
    let config = crate::config::Config::load().map_err(|e|e.message)?;
    let permission_mode = crate::accounts::permission_mode(&config,
        &nonempty_env("HGS_ACCOUNT_ID").unwrap_or_default(), &agent).map_err(|e|e.message)?;
    for argv in [&mut base, &mut first] { crate::accounts::apply_permissions(&agent, &permission_mode, argv); }
    // Claude Code reports its context window only to a status line command.
    // The saved base stays as launched; resume adds the settings again.
    if agent == "claude" {
        if let Some(settings) = claude_statusline::launch_settings(&base[0], &cwd, &[&base, &first]) {
            for argv in [&mut base, &mut first] {
                claude_statusline::add_settings(argv, &settings);
            }
        }
    }
    if agent == "codex" {
        for argv in [&mut base, &mut first] {
            let delimiter = argv
                .iter()
                .position(|arg| arg == "--")
                .unwrap_or(argv.len());
            if codex_no_daemon && !argv.is_empty() && !argv[..delimiter].iter().any(|arg| arg == "--no-daemon") {
                // Anything after -- is prompt text, never a CLI option.
                argv.insert(delimiter, "--no-daemon".into());
            }
        }
        eprintln!("hgs: tracking needs trusted hooks (review /hooks if prompted)");
    }
    if !first.is_empty() {
        let started = Instant::now();
        let mut child = spawn_agent(&first, &name, &token)?;
        let child_id = child.id();
        let status = supervisor::wait(&mut child)?;
        let confirmed = {
            let _guard = lock(None)?;
            let current =
                read_run(&name, &token)?.ok_or("session changed while launching agent")?;
            !string(&current, "conversation_id").is_empty() || current.get("pausing").is_some()
        };
        let fallback = nonempty_env("HGS_FALLBACK_SECS")
            .unwrap_or_else(|| "5".into())
            .parse::<f64>()
            .map_err(|_| "invalid HGS_FALLBACK_SECS")?;
        if status.success()
            || supervisor::shutdown_requested()
            || ExitInfo::of(status).signal.is_some()
            || confirmed
            || started.elapsed().as_secs_f64() >= fallback
        {
            supervisor::finish(&name, &token, child_id, status)?;
            return Ok(ExitInfo::of(status).exit_code());
        }
        eprintln!("hgs: nothing to resume — starting fresh");
        {
            let _guard = lock(None)?;
            let mut current =
                read_run(&name, &token)?.ok_or("session changed while launching agent")?;
            current["pid"] = json!(std::process::id());
            current["process_start"] = json!(process_start(std::process::id()));
            current["activity"] = json!("unknown");
            current["conversation_id"] = Value::Null;
            current["transcript"] = Value::Null;
            current.as_object_mut().unwrap().remove("cleared_conversations");
            current["startup_kind"] = json!("new");
            write(&mut current)?;
        }
    }
    let mut child = spawn_agent(&base, &name, &token)?;
    let child_id = child.id();
    let status = supervisor::wait(&mut child)?;
    supervisor::finish(&name, &token, child_id, status)?;
    Ok(ExitInfo::of(status).exit_code())
}

fn nonempty_value(record: &Value, key: &str) -> Option<String> {
    record[key]
        .as_str()
        .filter(|value| !value.is_empty())
        .map(str::to_owned)
}

fn spawn_agent(argv: &[String], name: &str, run_id: &str) -> Result<std::process::Child> {
    match supervisor::spawn(argv, name, run_id) {
        Ok(child) => Ok(child),
        Err(error) => {
            let _guard = lock(None)?;
            if let Some(mut record) = read_run(name, run_id)? {
                record["error"] = json!(error);
                write(&mut record)?;
                archive::failed_restore(&record)?;
            }
            Err(error)
        }
    }
}

pub(super) fn safe_to_pause(record: &Value) -> bool {
    pausable(record, || resume_input::idle(record))
}

/// `safe_to_pause` with the verification of a pending resume supplied by the
/// caller; it is asked only when no confirmed idle conversation qualifies.
pub(super) fn pausable(record: &Value, resumed_idle: impl FnOnce() -> bool) -> bool {
    !telemetry::work_remains(record)
        && ((string(record, "activity") == "idle" && string(record, "expected_id").is_empty())
            // Codex defers SessionStart after resume until the next turn; a
            // verified resume waits at its restored conversation's empty composer.
            || resumed_idle())
}

fn pause_check(name: &str, snapshot: &Snapshot) -> Result<Value> {
    let record = read(name)?;
    if !matches(&record, snapshot.get(name)) {
        return Err(format!(
            "{name}: live pane does not match its saved binding"
        ));
    }
    if pause_active(&record) {
        return Err(format!("{name}: another pause is already in progress"));
    }
    if !safe_to_pause(&record) {
        return Err(format!(
            "{name}: agent is busy or unconfirmed; wait for its answer before pausing"
        ));
    }
    if !process_alive(&record) {
        return Err(format!("{name}: tracked process is no longer present"));
    }
    recipes::verify_history(&record, false)?;
    recipes::resume_argv(
        string(&record, "agent"),
        &recipes::argv(&record)?,
        string(&record, "conversation_id"),
    )?;
    Ok(record)
}

fn pause_one(record: &Value, scope: Option<&session_action::Scope>) -> Result<()> {
    let name = string(record, "name");
    {
        let _guard = lock(None)?;
        let current = read(name)?;
        if let Some(scope)=scope {scope.check(&current)?;}
        if current["run_id"] != record["run_id"]
            || !safe_to_pause(&current)
            || !matches(&current, live()?.get(name))
            || !process_alive(&current)
        {
            return Err("session changed while pausing".into());
        }
        let pid = current["pid"]
            .as_u64()
            .and_then(|pid| i32::try_from(pid).ok())
            .filter(|pid| *pid > 0)
            .ok_or("invalid tracked process ID")?;
        // Only signal the exact PID whose start identity was just checked.
        // Never target a process group, terminal input or the tmux server. On
        // Windows the pane's supervisor raises Ctrl+Break for the group it
        // shares with this agent, as spec D11 decides.
        if let Some(scope)=scope {scope.changing();}
        backend::current().request_exit(string(&current, "pane"), pid as u32)?;
    }
    let deadline = Instant::now() + Duration::from_secs(10);
    let remaining = loop {
        let snapshot = live()?;
        let panes = snapshot.get(name);
        if panes.is_none()
            || panes
                .map(|panes| {
                    panes.len() == 1 && panes[0].0 == string(record, "pane") && panes[0].1 == "1"
                })
                .unwrap_or(false)
        {
            break panes.is_some();
        }
        if !matches(record, panes) {
            return Err("session changed while pausing".into());
        }
        if Instant::now() >= deadline {
            return Err("agent did not exit after SIGTERM (no forced kill was sent)".into());
        }
        std::thread::sleep(Duration::from_millis(100));
    };
    recipes::verify_history(&read(name)?, false)?;
    {
        let _guard = lock(None)?;
        let mut current = read(name)?;
        if let Some(scope)=scope {scope.check(&current)?;}
        if current["run_id"] != record["run_id"] {
            return Err("session changed while pausing".into());
        }
        current["paused"] = json!(true);
        current.as_object_mut().unwrap().remove("pausing");
        write(&mut current)?;
    }
    if remaining {
        let _guard=lock(None)?;
        if let Some(scope)=scope {scope.check(&read(name)?)?;}
        // Revalidate the dead pane before deleting it: tmux may reuse IDs after
        // a server restart while a delayed pause command is still finishing.
        let snapshot = live()?;
        if snapshot
            .get(name)
            .map(|panes| {
                panes.len() == 1
                    && panes[0].0 == string(record, "pane")
                    && panes[0].1 == "1"
                    && panes[0].2 == string(record, "run_id")
            })
            .unwrap_or(false)
        {
            backend::current().kill_pane(string(record, "pane"))?;
        }
    }
    if let Some(scope)=scope {scope.result(name,string(record,"run_id"),string(record,"conversation_id"),None);} else {println!("hgs: paused {name}");}
    Ok(())
}

pub(super) fn pause(names: &[String], dry: bool) -> Result<i32> { pause_scoped(names,dry,None) }

pub(super) fn pause_scoped(names: &[String], dry: bool, scope: Option<&session_action::Scope>) -> Result<i32> {
    let snapshot = live()?;
    let names: Vec<String> = if names == ["--all"] {
        snapshot.keys().cloned().collect()
    } else {
        names.to_vec()
    };
    let mut saved: Vec<Value> = names
        .iter()
        .map(|name| pause_check(name, &snapshot))
        .collect::<Result<_>>()?;
    if dry {
        for record in &saved {
            println!(
                "hgs: would pause {} ({})",
                string(record, "name"),
                string(record, "conversation_id")
            );
        }
        return Ok(0);
    }
    let owner = json!({"pid": std::process::id(), "start": process_start(std::process::id())});
    {
        let _guard = lock(None)?;
        for record in &saved {
            let current = read(string(record, "name"))?;
            if let Some(scope)=scope {scope.check(&current)?;}
            if current["run_id"] != record["run_id"] || current["updated"] != record["updated"] {
                return Err(format!(
                    "{}: activity changed while checking; retry pause",
                    string(record, "name")
                ));
            }
        }
        for record in &mut saved {
            if let Some(scope)=scope {scope.changing();}
            record["pausing"] = owner.clone();
            write(record)?;
        }
    }
    let mut failures = Vec::new();
    for record in &saved {
        let name = string(record, "name");
        if let Err(error) = pause_one(record, scope) {
            failures.push(format!("{name}: {error}"));
        }
        let cleanup: Result<()> = (|| {
            let _guard = lock(None)?;
            if record_path(name).exists() {
                let mut current = read(name)?;
                if current["run_id"] == record["run_id"] {
                    if let Some(scope)=scope {scope.check(&current)?;}
                    current.as_object_mut().unwrap().remove("pausing");
                    write(&mut current)?;
                }
            }
            Ok(())
        })();
        if let Err(error) = cleanup {
            failures.push(format!("{name}: {error}"));
        }
    }
    if !failures.is_empty() {
        return Err(failures.join("; "));
    }
    Ok(0)
}

fn resume_spec(
    record: &Value,
    run_id: &str,
    archive_id: Option<&str>,
) -> Result<backend::LaunchSpec> {
    let argv = recipes::resume_argv(
        string(record, "agent"),
        &recipes::argv(record)?,
        string(record, "conversation_id"),
    )?;
    session_spec(
        record,
        effort::resume_argv(record, argv),
        run_id,
        archive_id,
        None,
    )
}

/// The saved recipe relaunched in its own tmux slot. `replaces` starts a new
/// conversation in place of that exact stopped run instead of resuming it.
fn session_spec(
    record: &Value,
    argv: Vec<String>,
    run_id: &str,
    archive_id: Option<&str>,
    replaces: Option<&str>,
) -> Result<backend::LaunchSpec> {
    let name = string(record, "name");
    if !(Path::new(string(record, "launch_dir")).is_dir() || archive_id.is_some() && super::worktree_lifecycle::restorable(record)) {
        return Err(format!(
            "{name}: directory is missing: {}",
            string(record, "launch_dir")
        ));
    }
    let executable = runner_executable()?;
    let hint_environment = crate::config::agent_hint_environment(string(record, "agent"));
    let launch = supervisor_command(record, &executable, argv, &hint_environment);
    let mut environment = BTreeMap::from([
        ("HGS_SESSION", name.to_owned()),
        ("HGS_AGENT", string(record, "agent").to_owned()),
        (
            "HGS_STATE_DIR",
            absolute_root()?.to_string_lossy().into_owned(),
        ),
        ("HGS_EXECUTABLE", executable),
        (
            "HGS_EXPECTED_ID",
            if replaces.is_some() { "" } else { string(record, "conversation_id") }.to_owned(),
        ),
        ("HGS_RUN_ID", run_id.into()),
        ("HGS_ARCHIVE_ID", archive_id.unwrap_or("").into()),
        ("HGS_REQUESTED_ID", "".into()),
        ("HGS_FRESH", if replaces.is_some() { "1" } else { "0" }.into()),
        ("HGS_FRESH_REPLACES", replaces.unwrap_or("").into()),
        ("HGS_TRACKING", "1".into()),
        (
            "HGS_CLIENT",
            nonempty_env("HGS_CLIENT").unwrap_or_else(|| "local".into()),
        ),
    ]);
    environment.insert(
        home_var(string(record, "agent"))?,
        string(record, "agent_home").to_owned(),
    );
    Ok(backend::LaunchSpec {
        name: name.into(),
        cwd: string(record, "launch_dir").to_owned(),
        env: environment
            .into_iter()
            .map(|(key, value)| format!("{key}={value}"))
            .chain(account_variables(record))
            .chain(hint_environment)
            .collect(),
        command: launch,
        detached: true,
    })
}

fn print_resume(spec: &backend::LaunchSpec) {
    println!(
        "{}",
        backend::current().describe(&backend::DryRun::Start(spec))
    );
}

fn resume_archive(name: &str, id: &str, dry: bool, scope: Option<&session_action::Scope>) -> Result<i32> {
    let archived = archive::read_archive(name, id)?;
    recipes::verify_history(&archived, true)?;
    if let Some(scope)=scope {scope.check(&archived)?;}
    {
        let _guard = lock(None)?;
        if live()?.contains_key(name) || record_path(name).exists() { return Err(format!("{name}: a live or saved session already uses this name; finish or archive it before restoring")); }
    }
    if !dry { super::worktree_lifecycle::restore(&archived)?; }
    let run_id = uuid::Uuid::new_v4().to_string();
    let spec = resume_spec(&archived, &run_id, Some(id))?;
    let _guard = lock(None)?;
    if live()?.contains_key(name) || record_path(name).exists() {
        return Err(format!("{name}: a live or saved session already uses this name; finish or archive it before restoring"));
    }
    let current_archive = archive::read_archive(name, id)?;
    if let Some(scope)=scope {scope.check(&current_archive)?;}
    if current_archive["run_id"] != archived["run_id"]
        || current_archive["conversation_id"] != archived["conversation_id"]
    {
        return Err("archived binding changed; retry resume".into());
    }
    if dry {
        print_resume(&spec);
        return Ok(0);
    }
    // Reserve the name while holding the same lock used by hooks and ordinary
    // launch. The original archive remains durable until exact confirmation.
    let mut pending = archived.clone();
    for key in [
        "archive_id",
        "archived_at",
        "state",
        "completion_source",
        "completion_reason",
        "exit_code",
        "termination_signal",
        "exited_at",
        "session_end",
        "paused",
        "error",
        "last_restore_error",
        "pausing",
    ] {
        pending.as_object_mut().unwrap().remove(key);
    }
    pending["run_id"] = json!(run_id);
    pending["restore_archive_id"] = json!(id);
    pending["resume_pending"] = json!(true);
    pending["expected_id"] = pending["conversation_id"].clone();
    pending["activity"] = json!("unknown");
    pending["phase"] = json!("unknown");
    pending["pid"] = json!(0);
    pending["process_start"] = json!("");
    if let Some(scope)=scope {scope.changing();}
    write(&mut pending)?;
    if let Err(error) = backend::current().start(&spec) {
        let mut failed = read(name)?;
        if string(&failed, "run_id") == run_id {
            failed["error"] = json!(error);
            archive::failed_restore(&failed)?;
        }
        return Err(error);
    }
    if let Some(scope)=scope {scope.result(name,&run_id,string(&pending,"conversation_id"),None);} else {println!("hgs: restoring {name} from archive {id}");}
    Ok(0)
}

pub(super) fn resume(names: &[String], dry: bool) -> Result<i32> { resume_scoped(names,dry,None) }

pub(super) fn resume_scoped(names: &[String], dry: bool, scope: Option<&session_action::Scope>) -> Result<i32> {
    if scope.is_none() {archive::reconcile()?;}
    if names.len() == 3 && names[1] == "--archive" {
        return resume_archive(&names[0], &names[2], dry, scope);
    }
    if names.iter().any(|name| name == "--archive") {
        return Err("usage: hgs resume <session> --archive ID".into());
    }
    let snapshot = live()?;
    let requested = if scope.is_none() {nonempty_env("HGS_REQUESTED_ID")} else {None};
    let archived = archive::records()?;
    let names = if names == ["--all"] {
        records()?
            .iter()
            .filter(|record| {
                !snapshot.contains_key(string(record, "name"))
                    && !string(record, "conversation_id").is_empty()
                    && !archive::equivalent(record, &archived)
            })
            .map(|record| string(record, "name").to_owned())
            .collect::<Vec<_>>()
    } else {
        names.to_vec()
    };
    let mut recipes = Vec::new();
    for name in names {
        if let Some(id) = &requested {
            verify_requested(&read(&name)?, &snapshot, id)?;
        }
        if snapshot.contains_key(&name) {
            if scope.is_some() {return Err("Session is already live; no resume was launched".into());}
            continue;
        }
        let record = read(&name)?;
        if let Some(scope)=scope {scope.check(&record)?;}
        if archive::equivalent(&record, &archived) {
            return Err(format!(
                "{name}: this session is archived; restore it using --archive ID"
            ));
        }
        recipes::verify_history(&record, true)?;
        let restore_id = nonempty_value(&record, "restore_archive_id");
        let new_run=uuid::Uuid::new_v4().to_string();
        let spec = resume_spec(&record, &new_run, restore_id.as_deref())?;
        recipes.push((name, spec, record["run_id"].clone(), record["conversation_id"].clone(),new_run));
    }
    let _guard = lock(None)?;
    for (name, spec, original_run, original_conversation,new_run) in recipes {
        let current_snapshot = live()?;
        let current = read(&name)?;
        if let Some(scope)=scope {scope.check(&current)?;}
        if let Some(id) = &requested {
            verify_requested(&current, &current_snapshot, id)?;
        }
        if current_snapshot.contains_key(&name) {
            if scope.is_some() {return Err("Session became live before resume; no replacement was launched".into());}
            continue;
        }
        if current["run_id"] != original_run || current["conversation_id"] != original_conversation {
            return Err(format!("{name}: saved binding changed; retry resume"));
        }
        if dry {
            print_resume(&spec);
        } else {
            if let Some(scope)=scope {scope.changing();}
            backend::current().start(&spec)?;
            if let Some(scope)=scope {scope.result(&name,&new_run,string(&current,"conversation_id"),None);} else {println!("hgs: resuming {name}");}
        }
    }
    Ok(0)
}

/// `hgs resume <session> --fresh`: the stopped session's own launch recipe,
/// account and model with a new conversation, like /clear in a live agent.
/// The earlier conversation stays in native history and above the boundary.
pub(super) fn fresh(args: &[String], dry: bool) -> Result<i32> {
    let (name, expected_run) = match args {
        [name] => (name, None),
        [name, flag, run] if flag == "--expected-run-id" && !run.is_empty() => (name, Some(run)),
        _ => return Err("usage: hgs resume <session> --fresh [--expected-run-id ID] [-d]".into()),
    };
    archive::reconcile()?;
    let record = read(name)?;
    if !AGENTS.contains(&string(&record, "agent")) {
        return Err(format!("{name}: only Claude, Codex and Kimi sessions can start fresh"));
    }
    let argv = recipes::fresh_argv(string(&record, "agent"), &recipes::argv(&record)?)?;
    let run_id = uuid::Uuid::new_v4().to_string();
    let spec = session_spec(
        &record,
        effort::resume_argv(&record, argv),
        &run_id,
        None,
        Some(string(&record, "run_id")),
    )?;
    let _guard = lock(None)?;
    let current = read(name)?;
    if live()?.contains_key(name) {
        return Err(format!("{name}: the session is running; clear its context instead"));
    }
    if expected_run.is_some_and(|run| run != string(&current, "run_id"))
        || current["run_id"] != record["run_id"]
        || current["conversation_id"] != record["conversation_id"]
    {
        return Err(format!("{name}: saved binding changed; refresh before starting fresh"));
    }
    if archive::equivalent(&current, &archive::records()?) {
        return Err(format!("{name}: this session is archived; restore it using --archive ID"));
    }
    if pause_active(&current) || current["resume_pending"] == true {
        return Err(format!("{name}: wait for the current pause or restore to finish"));
    }
    if dry {
        print_resume(&spec);
        return Ok(0);
    }
    backend::current().start(&spec)?;
    println!("hgs: starting {name} with a fresh conversation");
    Ok(0)
}

fn verify_requested(record: &Value, snapshot: &Snapshot, requested: &str) -> Result<()> {
    let name = string(record, "name");
    let agent = string(record, "agent");
    if string(record, "conversation_id") != requested
        || name.split('/').next() != Some(agent)
        || !attempts::agent_home_matches(record, agent)?
        || (snapshot.contains_key(name)
            && (!matches(record, snapshot.get(name)) || !string(record, "error").is_empty()))
    {
        return Err(format!(
            "{name}: saved or live binding no longer matches requested conversation {requested}"
        ));
    }
    Ok(())
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;

    #[test]
    fn capability_detection_never_runs_a_project_launcher() {
        use std::os::unix::fs::PermissionsExt;
        let folder = tempfile::tempdir().unwrap();
        let marker = folder.path().join("launcher-ran");
        let launcher = folder.path().join("run_codex.sh");
        std::fs::write(&launcher, format!("#!/bin/sh\n: > '{}'\nexit 7\n", marker.display())).unwrap();
        std::fs::set_permissions(&launcher, std::fs::Permissions::from_mode(0o755)).unwrap();
        assert_eq!(codex_daemon_flag(&launcher.to_string_lossy()), Ok(true));
        assert!(!marker.exists());
    }
}

#[cfg(test)]
mod launcher_tests {
    use super::*;

    /// A `.ps1` project launcher's program is PowerShell, which is never asked
    /// for Codex's capabilities: like a `.sh` launcher it gets `--no-daemon`.
    #[test]
    fn powershell_launchers_get_no_daemon_without_a_capability_check() {
        for program in [r"C:\Program Files\PowerShell\7\pwsh.exe", "/usr/bin/pwsh", "pwsh"] {
            assert_eq!(codex_daemon_flag(program), Ok(true), "{program}");
        }
    }
}
