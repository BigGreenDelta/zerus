use super::*;
use std::process::{Command, Output};
use std::time::Duration;

pub(super) fn argv(record: &Value) -> Result<Vec<String>> {
    let values = record["base"]
        .as_array()
        .ok_or("saved launch arguments are missing")?;
    let result: Vec<String> = values
        .iter()
        .map(|value| {
            value
                .as_str()
                .map(str::to_owned)
                .ok_or_else(|| "invalid launch argument".into())
        })
        .collect::<Result<_>>()?;
    if result.first().map(|s| s.is_empty()).unwrap_or(true) {
        return Err("saved agent executable is missing".into());
    }
    Ok(result)
}

pub(super) fn resume_argv(agent: &str, argv: &[String], sid: &str) -> Result<Vec<String>> {
    let value_flags: &[&str] = match agent {
        "claude" => &[
            "--model",
            "--effort",
            "--agent",
            "--agents",
            "--append-system-prompt",
            "--system-prompt",
            "--settings",
            "--setting-sources",
            "--permission-mode",
            "--permission-prompt-tool",
            "--remote-control",
            "--mcp-config",
            "--max-budget-usd",
            "--fallback-model",
            "--debug-file",
            "--betas",
            "--system-prompt-snapshot",
        ],
        "codex" => &[
            "-c",
            "--config",
            "-m",
            "--model",
            "-p",
            "--profile",
            "-s",
            "--sandbox",
            "-a",
            "--ask-for-approval",
            "-C",
            "--cd",
            "--enable",
            "--disable",
            "--local-provider",
        ],
        "kimi" => &[
            "-m",
            "--model",
            "--agent",
            "--agent-file",
            "--skills-dir",
            "--add-dir",
        ],
        _ => return Err(format!("unsupported agent: {agent}")),
    };
    let multi_flags: &[&str] = match agent {
        "claude" => &[
            "--add-dir",
            "--allowedTools",
            "--allowed-tools",
            "--disallowedTools",
            "--disallowed-tools",
            "--tools",
            "--plugin-dir",
        ],
        "codex" => &["--add-dir"],
        _ => &[],
    };
    let bool_flags: &[&str] = match agent {
        "claude" => &[
            "--verbose",
            "--dangerously-skip-permissions",
            "--allow-dangerously-skip-permissions",
            "--strict-mcp-config",
            "--disable-slash-commands",
            "--ide",
            "--chrome",
            "--no-chrome",
        ],
        "codex" => &[
            "--no-daemon",
            "--no-alt-screen",
            "--oss",
            "--search",
            "--approve-for-me",
            "--dangerously-bypass-approvals-and-sandbox",
            "--dangerously-bypass-hook-trust",
            "--strict-config",
            "--include-non-interactive",
        ],
        "kimi" => &["-y", "--yolo", "--auto", "--plan"],
        _ => &[],
    };
    if argv.is_empty() {
        return Err("saved agent executable is missing".into());
    }
    // The program: the agent, a `.sh` launcher or a `.ps1` launcher's PowerShell
    // command line, whose agent arguments follow it.
    let program = crate::project_launcher::program_words(argv);
    let mut result = argv[..program].to_vec();
    result.extend([
        match agent {
            "codex" => "resume",
            "claude" => "--resume",
            _ => "--session",
        }
        .into(),
        sid.into(),
    ]);
    let mut index = program;
    while index < argv.len() {
        let argument = &argv[index];
        let key = argument.split('=').next().unwrap_or(argument);
        if argument == "--" {
            break;
        }
        if agent == "codex" && ["resume", "fork"].contains(&argument.as_str()) {
            index += 1;
            if index < argv.len() && !argv[index].starts_with('-') {
                index += 1;
            }
            continue;
        }
        if agent == "codex" && ["exec", "review", "app-server"].contains(&argument.as_str()) {
            return Err("only interactive Codex sessions support pause/resume".into());
        }
        if agent != "codex" && ["-r", "--resume", "-S", "--session", "--session-id"].contains(&key)
        {
            index += 1;
            if !argument.contains('=') && index < argv.len() && !argv[index].starts_with('-') {
                index += 1;
            }
            continue;
        }
        if (agent == "codex" && ["--last", "--all"].contains(&argument.as_str()))
            || (agent != "codex"
                && ["-c", "--continue", "--fork-session"].contains(&argument.as_str()))
        {
            index += 1;
            continue;
        }
        if value_flags.contains(&key) || multi_flags.contains(&key) {
            result.push(argument.clone());
            index += 1;
            if argument.contains('=') {
                continue;
            }
            if key == "--remote-control" && (index == argv.len() || argv[index].starts_with('-')) {
                continue;
            }
            if index == argv.len() {
                return Err(format!("missing value for {argument}"));
            }
            result.push(argv[index].clone());
            index += 1;
            if agent == "claude" && multi_flags.contains(&key) {
                while index < argv.len() && !argv[index].starts_with('-') {
                    result.push(argv[index].clone());
                    index += 1;
                }
            }
            continue;
        }
        if bool_flags.contains(&argument.as_str()) {
            result.push(argument.clone());
        } else if argument.starts_with('-') {
            return Err(format!(
                "cannot safely restore launch option {argument}; this option is not supported yet"
            ));
        }
        // Other positional arguments are the original prompt, never replayed.
        index += 1;
    }
    Ok(result)
}

/// The resume recipe keeps only safe launch options and never replays the
/// original prompt; without its selector it starts a new conversation.
pub(super) fn fresh_argv(agent: &str, argv: &[String]) -> Result<Vec<String>> {
    let program = crate::project_launcher::program_words(argv);
    let mut argv = resume_argv(agent, argv, "-")?;
    argv.drain(program..program + 2);
    Ok(argv)
}

pub(super) fn output_timeout(command: &mut Command, duration: Duration) -> Result<Output> {
    // Regular files avoid a blocked pipe when an agent writes substantial
    // diagnostics before it exits, without an unbounded reader-thread join.
    crate::platform::output_until(command, duration).map_err(|error| match error {
        crate::platform::RunError::Spawn(e) | crate::platform::RunError::Io(e) => e.to_string(),
        _ => "timed out verifying agent history".into(),
    })
}

pub(super) fn verify_history(record: &Value, allow_error: bool) -> Result<()> {
    let name = string(record, "name");
    let conversation = string(record, "conversation_id");
    if conversation.is_empty() {
        return Err(format!(
            "{name}: conversation ID is not confirmed; check agent hooks"
        ));
    }
    if !allow_error && !string(record, "error").is_empty() {
        return Err(format!("{name}: {}", string(record, "error")));
    }
    if string(record, "agent") == "kimi" {
        let mut command = Command::new("kimi");
        command.args(["session", "list", "--all", "--json"]);
        if string(record, "agent_home").is_empty() {
            command.env_remove("KIMI_CODE_HOME");
        } else {
            command.env("KIMI_CODE_HOME", string(record, "agent_home"));
        }
        let output = output_timeout(&mut command, Duration::from_secs(10))?;
        if !output.status.success() {
            return Err(format!(
                "cannot verify Kimi history: {}",
                String::from_utf8_lossy(&output.stderr).trim()
            ));
        }
        let sessions: Value = serde_json::from_slice(&output.stdout).map_err(|e| e.to_string())?;
        if !sessions
            .as_array()
            .ok_or("invalid Kimi session list")?
            .iter()
            .any(|session| string(session, "id") == conversation)
        {
            return Err(format!("{name}: saved Kimi conversation no longer exists"));
        }
    } else {
        let path = Path::new(string(record, "transcript"));
        if !path
            .metadata()
            .map(|metadata| metadata.is_file() && metadata.len() > 0)
            .unwrap_or(false)
        {
            return Err(format!(
                "{name}: persisted conversation is missing or empty"
            ));
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn strings(values: &[&str]) -> Vec<String> {
        values.iter().map(|value| (*value).to_owned()).collect()
    }
    #[test]
    fn fresh_launches_keep_options_without_conversation_or_prompt() {
        assert_eq!(
            fresh_argv("codex", &strings(&["codex", "resume", "old", "-m", "gpt-6-astra", "prompt"])).unwrap(),
            strings(&["codex", "-m", "gpt-6-astra"])
        );
        assert_eq!(
            fresh_argv("claude", &strings(&["claude", "--resume", "old", "--effort", "high", "--fork-session"])).unwrap(),
            strings(&["claude", "--effort", "high"])
        );
        assert_eq!(
            fresh_argv("kimi", &strings(&["kimi", "--session", "old", "--plan"])).unwrap(),
            strings(&["kimi", "--plan"])
        );
        assert!(fresh_argv("codex", &strings(&["codex", "--unknown"])).is_err());
    }

    /// A `.ps1` project launcher's saved recipe keeps its whole PowerShell
    /// command line in front, on Windows and on Unix alike.
    #[test]
    fn powershell_launcher_recipes_resume_after_the_script_path() {
        let windows = [
            r"C:\Program Files\PowerShell\7\pwsh.exe", "-NoLogo", "-NoProfile",
            "-ExecutionPolicy", "Bypass", "-File", r"C:\p\run_codex.ps1",
        ];
        let unix = ["/usr/bin/pwsh", "-NoLogo", "-NoProfile", "-File", "/p/run_codex.ps1"];
        for launcher in [&windows[..], &unix[..]] {
            let with = |args: &[&str]| [strings(launcher), strings(args)].concat();
            assert_eq!(
                resume_argv("codex", &with(&["resume", "--last", "--no-daemon", "-m", "gpt-6-astra", "prompt"]), "id").unwrap(),
                with(&["resume", "id", "--no-daemon", "-m", "gpt-6-astra"])
            );
            assert_eq!(
                fresh_argv("codex", &with(&["resume", "old", "-c", "model=\"m\""])).unwrap(),
                with(&["-c", "model=\"m\""])
            );
            assert_eq!(
                resume_argv("claude", &with(&["-c", "--effort", "high"]), "id").unwrap(),
                with(&["--resume", "id", "--effort", "high"])
            );
            assert_eq!(
                resume_argv("kimi", &with(&["--plan"]), "id").unwrap(),
                with(&["--session", "id", "--plan"])
            );
            assert_eq!(fresh_argv("claude", &with(&[])).unwrap(), with(&[]));
            assert!(resume_argv("codex", &with(&["--unknown"]), "id").is_err());
        }
        // A `.sh` launcher is one word, like the native CLI.
        assert_eq!(
            resume_argv("claude", &strings(&["/p/run_claude.sh", "-c"]), "id").unwrap(),
            strings(&["/p/run_claude.sh", "--resume", "id"])
        );
    }
}
