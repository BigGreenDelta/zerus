//! A provider-native branch creates a different conversation and a different
//! tmux binding. No input, resume, termination, or rename touches the source.
use super::*;
use std::process::Command;
use std::time::Duration;

fn unavailable(record: &Value, live_pane: bool) -> Option<&'static str> {
    if !AGENTS.contains(&string(record, "agent")) {
        return Some("This provider does not support session forks");
    }
    if string(record, "conversation_id").is_empty()
        || !string(record, "expected_id").is_empty()
        || !string(record, "error").is_empty()
    {
        return Some("Wait for a confirmed conversation before forking");
    }
    if live_pane && (!telemetry::settled(record) || pause_active(record)) {
        return Some("Wait for Ready before forking the saved context");
    }
    if record["base"].as_array().is_none() {
        return Some("Saved launch arguments are unavailable");
    }
    None
}

pub(super) fn summary(record: &Value, live_pane: bool) -> Value {
    let reason = unavailable(record, live_pane).unwrap_or("");
    json!({"fork_supported":reason.is_empty(),"fork_reason":reason,
        "fork_parent_id":record.get("fork_parent_id").unwrap_or(&Value::Null),
        "fork_source_name":record.get("fork_source_name").unwrap_or(&Value::Null)})
}

/// A live session's summary while a chooser that only the terminal shows
/// waits for an answer. Its saved record stays idle, but the fork's
/// `input::checked_ready` refuses the chooser, so the fork and the listed fork
/// action wait for it. An earlier reason keeps its precedence.
pub(super) fn wait_for_chooser(output: &mut Value, reason: &str) {
    if output["fork_supported"] != true {
        return;
    }
    output["fork_supported"] = json!(false);
    output["fork_reason"] = json!(reason);
    if let Some(actions) = output["allowed_actions"].as_array_mut() {
        actions.retain(|action| action != "fork");
    }
    if let Some(reasons) = output["action_reasons"].as_object_mut() {
        reasons.insert("fork".into(), json!(reason));
    }
}

fn fork_argv(record: &Value, new_id: Option<&str>) -> Result<Vec<String>> {
    let agent = string(record, "agent");
    let saved = recipes::argv(record)?;
    let mut argv = recipes::resume_argv(
        agent,
        &saved,
        new_id.unwrap_or_else(|| string(record, "conversation_id")),
    )?;
    match agent {
        // `resume` follows the program, which a `.ps1` launcher spreads over words.
        "codex" => argv[crate::project_launcher::program_words(&saved)] = "fork".into(),
        "claude" => argv.push("--fork-session".into()),
        "kimi" => {}
        _ => return Err("this provider does not support session forks".into()),
    }
    Ok(argv)
}

fn token(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 200
        && value
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || c == b'-' || c == b'_')
}

fn parse_kimi_id(text: &str, parent: &str) -> Result<String> {
    let values: Vec<_> = text
        .lines()
        .filter_map(|line| line.strip_prefix("Forked to "))
        .filter_map(|line| line.split_whitespace().next())
        .collect();
    match values.as_slice() {
        [id] if token(id) && *id != parent => Ok((*id).into()),
        _ => Err(
            "Kimi did not return a distinct fork ID; inspect its session list before retrying"
                .into(),
        ),
    }
}

fn kimi_fork(record: &Value) -> Result<String> {
    // Use the native CLI, not a launcher script whose behavior might wrap an
    // interactive agent. The exact home and source ID bind the source history.
    let mut command = Command::new("kimi");
    command
        .args(["fork", string(record, "conversation_id"), "--yes"])
        .current_dir(string(record, "launch_dir"));
    if string(record, "agent_home").is_empty() {
        command.env_remove("KIMI_CODE_HOME");
    } else {
        command.env("KIMI_CODE_HOME", string(record, "agent_home"));
    }
    for key in [
        "HGS_SESSION",
        "HGS_RUN_ID",
        "HGS_AGENT",
        "HGS_REQUESTED_ID",
        "HGS_EXPECTED_ID",
    ] {
        command.env_remove(key);
    }
    let output = recipes::output_timeout(&mut command, Duration::from_secs(30))?;
    if !output.status.success() {
        return Err(format!(
            "Kimi fork failed: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        ));
    }
    parse_kimi_id(
        &String::from_utf8_lossy(&output.stdout),
        string(record, "conversation_id"),
    )
}

pub(super) fn choose_name(
    source: &str,
    tag: Option<&str>,
    occupied: &std::collections::BTreeSet<String>,
) -> Result<String> {
    let parts: Vec<_> = source.splitn(3, '/').collect();
    if parts.len() < 2
        || parts
            .iter()
            .any(|s| s.is_empty() || s.contains([':', '.']) || s.chars().any(char::is_control))
    {
        return Err("source session has an invalid name".into());
    }
    let prefix = format!("{}/{}/", parts[0], parts[1]);
    if let Some(tag) = tag {
        if tag.is_empty()
            || tag.len() > 120
            || tag.trim() != tag
            || tag.contains(['/', ':', '.'])
            || tag.chars().any(char::is_control)
        {
            return Err("fork name must be a nonempty tag without '/', ':', '.', control characters or surrounding whitespace".into());
        }
        let name = format!("{prefix}{tag}");
        if occupied.contains(&name) {
            return Err(format!("{name}: a session already uses this name"));
        }
        return Ok(name);
    }
    let base = format!("{}-fork", parts.get(2).copied().unwrap_or("session"));
    for suffix in 1..10000 {
        let name = if suffix == 1 {
            format!("{prefix}{base}")
        } else {
            format!("{prefix}{base}-{suffix}")
        };
        if !occupied.contains(&name) {
            return Ok(name);
        }
    }
    Err("could not find an unused fork name".into())
}

pub(super) fn start(
    name: &str,
    tag: Option<&str>,
    archive_id: Option<&str>,
    expected_run: Option<&str>,
    expected_conversation: Option<&str>,
    dry: bool,
) -> Result<String> {
    start_scoped(name,tag,archive_id,expected_run,expected_conversation,dry,None)
}

pub(super) fn start_scoped(
    name: &str,
    tag: Option<&str>,
    archive_id: Option<&str>,
    expected_run: Option<&str>,
    expected_conversation: Option<&str>,
    dry: bool,
    scope: Option<&session_action::Scope>,
) -> Result<String> {
    if archive_id.is_none() && dsh::exists(name) {
        return dsh::fork(name, tag, expected_run, expected_conversation, dry, scope);
    }
    let _guard = lock(None)?;
    let record = if let Some(id) = archive_id {
        archive::read_archive(name, id)?
    } else {
        read(name)?
    };
    if let Some(scope)=scope {scope.check(&record)?;}
    if expected_run.is_some_and(|id| id != string(&record, "run_id"))
        || expected_conversation.is_some_and(|id| id != string(&record, "conversation_id"))
    {
        return Err("source session changed; refresh before forking".into());
    }
    let snapshot = live()?;
    let live_pane = archive_id.is_none() && snapshot.contains_key(name);
    if live_pane && (!matches(&record, snapshot.get(name)) || !process_alive(&record)) {
        return Err("source process no longer matches its tracked session".into());
    }
    if let Some(reason) = unavailable(&record, live_pane) {
        return Err(reason.into());
    }
    if live_pane {
        input::checked_ready(
            name,
            string(&record, "run_id"),
            string(&record, "conversation_id"),
        )?;
    }
    recipes::verify_history(&record, archive_id.is_some())?;
    if !Path::new(string(&record, "launch_dir")).is_dir() {
        return Err("the source working directory is missing".into());
    }
    let mut occupied: std::collections::BTreeSet<String> = snapshot.keys().cloned().collect();
    for record in records()?.into_iter().chain(archive::records()?) {
        occupied.insert(string(&record, "name").into());
    }
    let target = choose_name(name, tag, &occupied)?;
    let agent = string(&record, "agent");
    // Check the saved recipe before the only operation that can create history.
    fork_argv(&record, None)?;
    let new_id = if agent == "kimi" {
        if dry {
            Some("NEW_KIMI_FORK_ID".to_owned())
        } else {
            if let Some(scope)=scope {scope.changing();}
            Some(kimi_fork(&record)?)
        }
    } else {
        None
    };
    let argv = fork_argv(&record, new_id.as_deref())?;
    let executable = runner_executable()?;
    let hint_environment = crate::config::agent_hint_environment(agent);
    let launch = supervisor_command(&record, &executable, argv, &hint_environment);
    let new_run=uuid::Uuid::new_v4().to_string();
    let mut environment = BTreeMap::from([
        ("HGS_SESSION", target.clone()),
        ("HGS_AGENT", agent.into()),
        (
            "HGS_STATE_DIR",
            absolute_root()?.to_string_lossy().into_owned(),
        ),
        ("HGS_EXECUTABLE", executable),
        ("HGS_RUN_ID", new_run.clone()),
        ("HGS_EXPECTED_ID", String::new()),
        ("HGS_REQUESTED_ID", new_id.clone().unwrap_or_default()),
        ("HGS_ARCHIVE_ID", String::new()),
        ("HGS_FRESH", "1".into()),
        ("HGS_FRESH_REPLACES", String::new()),
        (
            "HGS_FORK_PARENT_ID",
            string(&record, "conversation_id").into(),
        ),
        ("HGS_FORK_SOURCE_NAME", name.into()),
        (agent_hint::FORK_REVISION_ENV, agent_hint::fork_revision(&record)),
        ("HGS_TRACKING", "1".into()),
        (
            "HGS_CLIENT",
            nonempty_env("HGS_CLIENT").unwrap_or_else(|| "local".into()),
        ),
    ]);
    for key in ["CLAUDE_CONFIG_DIR", "CODEX_HOME", "KIMI_CODE_HOME"] {
        environment.insert(key, String::new());
    }
    environment.insert(home_var(agent)?, string(&record, "agent_home").into());
    let spec = backend::LaunchSpec {
        name: target.clone(),
        cwd: string(&record, "launch_dir").into(),
        env: environment
            .into_iter()
            .map(|(key, value)| format!("{key}={value}"))
            .chain(account_variables(&record))
            .chain(hint_environment)
            .collect(),
        command: launch,
        detached: true,
    };
    if dry {
        if agent == "kimi" {
            println!(
                "{}",
                join_command(&[
                    "kimi".into(),
                    "fork".into(),
                    string(&record, "conversation_id").into(),
                    "--yes".into()
                ])
            );
        }
        println!("{}", backend::current().describe(&backend::DryRun::Start(&spec)));
    } else {
        if let Some(scope)=scope {scope.changing();}
        if let Err(error) = backend::current().start(&spec) {
        return Err(if let Some(id) = &new_id {
            format!("{error}; Kimi fork {id} is saved and can be opened explicitly")
        } else {
            error
        });
        } else if scope.is_none() {println!("hgs: forked {name} as {target}");}
        if let (Some(scope),Some(id))=(scope,new_id.as_deref()) {scope.result(&target,&new_run,id,None);}
    }
    Ok(target)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn live_forks_wait_for_the_ready_state_the_listing_reports() {
        let mut stale = telemetry::stale_hook_child();
        stale["base"] = json!(["kimi"]);
        assert_eq!(unavailable(&stale, true), None);
        for (case, mut record) in telemetry::remaining_work() {
            record["base"] = json!(["kimi"]);
            assert_eq!(unavailable(&record, true), Some("Wait for Ready before forking the saved context"), "{case}");
            // A stopped session forks its saved context.
            assert_eq!(unavailable(&record, false), None, "{case}");
        }
    }
    #[test]
    fn a_terminal_chooser_keeps_earlier_fork_reasons() {
        let reason = "Wait until the agent's question is answered";
        let mut stale = telemetry::stale_hook_child();
        stale["base"] = json!(["kimi"]);
        let mut output = summary(&stale, true);
        output["allowed_actions"] = json!(["rename", "fork"]);
        output["action_reasons"] = json!({});
        wait_for_chooser(&mut output, reason);
        assert_eq!((&output["fork_supported"], &output["fork_reason"]), (&json!(false), &json!(reason)));
        assert_eq!(output["allowed_actions"], json!(["rename"]));
        assert_eq!(output["action_reasons"], json!({"fork": reason}));
        // A trust or update panel before the first message: the conversation
        // is unconfirmed, which stays the reason.
        stale["conversation_id"] = json!("");
        let mut output = summary(&stale, true);
        wait_for_chooser(&mut output, reason);
        assert_eq!(output, summary(&stale, true));
        assert_eq!(output["fork_reason"], "Wait for a confirmed conversation before forking");
    }
    #[test]
    fn forks_use_exact_parent_and_preserve_safe_launch_options() {
        let record = |agent, base| json!({"agent":agent,"base":base,"conversation_id":"parent"});
        assert_eq!(
            fork_argv(
                &record(
                    "codex",
                    json!([
                        "codex",
                        "resume",
                        "old",
                        "-m",
                        "gpt-6-astra",
                        "prompt must not replay"
                    ])
                ),
                None
            )
            .unwrap(),
            vec!["codex", "fork", "parent", "-m", "gpt-6-astra"]
        );
        assert_eq!(
            fork_argv(
                &record(
                    "claude",
                    json!(["claude", "--resume", "old", "--effort", "high"])
                ),
                None
            )
            .unwrap(),
            vec![
                "claude",
                "--resume",
                "parent",
                "--effort",
                "high",
                "--fork-session"
            ]
        );
        assert_eq!(
            fork_argv(
                &record("kimi", json!(["kimi", "--session", "old", "--plan"])),
                Some("new")
            )
            .unwrap(),
            vec!["kimi", "--session", "new", "--plan"]
        );
        // A `.ps1` project launcher forks through its own PowerShell command line.
        let launcher = [r"C:\pwsh.exe", "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass",
            "-File", r"C:\p\run_codex.ps1"];
        let mut saved = launcher.to_vec();
        saved.extend(["resume", "old", "--no-daemon", "prompt"]);
        let mut forked = launcher.to_vec();
        forked.extend(["fork", "parent", "--no-daemon"]);
        assert_eq!(fork_argv(&record("codex", json!(saved)), None).unwrap(), forked);
        let mut saved = launcher.to_vec();
        saved.push("-c");
        let mut forked = launcher.to_vec();
        forked.extend(["--resume", "parent", "--fork-session"]);
        assert_eq!(fork_argv(&record("claude", json!(saved)), None).unwrap(), forked);
    }
    #[test]
    fn unique_names_and_distinct_kimi_ids() {
        let used = [
            "codex/project/work-fork".into(),
            "codex/project/work-fork-2".into(),
        ]
        .into_iter()
        .collect();
        assert_eq!(
            choose_name("codex/project/work", None, &used).unwrap(),
            "codex/project/work-fork-3"
        );
        assert!(choose_name("codex/project/work", Some("work-fork"), &used).is_err());
        assert!(choose_name("codex/project/work", Some("../bad"), &used).is_err());
        assert_eq!(
            parse_kimi_id(
                "Forked to session_new (\"title\") in 20ms\n",
                "session_parent"
            )
            .unwrap(),
            "session_new"
        );
        assert!(parse_kimi_id("Forked to session_parent in 1ms", "session_parent").is_err());
        assert!(parse_kimi_id("random output", "session_parent").is_err());
    }
}
