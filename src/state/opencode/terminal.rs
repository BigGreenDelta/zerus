//! The native TUI is an attach client of the existing shared service. It is
//! deliberately separate from `hgs terminal`'s pane-snapshot protocol.
use super::*;
#[cfg(unix)]
use std::process::Command;

#[cfg(unix)]
fn command(
    record: &Value,
    reg: &service::Registration,
    expected_run: Option<&str>,
    environment: Vec<(String, String)>,
) -> Result<Command> {
    let run = string(record, "run_id");
    if expected_run.is_some_and(|expected| expected != run) {
        return Err(backend::RUN_CHANGED.into());
    }
    if record["service"] != "opencode2"
        || run.is_empty()
        || !native_id(string(record, "conversation_id"), "ses_")
        || !Path::new(string(record, "cwd")).is_absolute()
        || !Path::new(string(record, "agent_home")).is_absolute()
        || record["creation_pending"] == true
    {
        return Err("Refresh the native OpenCode session before opening Terminal".into());
    }
    let mut result = Command::new("opencode");
    result
        .args([
            "--server",
            &format!("http://127.0.0.1:{}", reg.port),
            "--session",
            string(record, "conversation_id"),
        ])
        .current_dir(string(record, "cwd"))
        .env_clear()
        .envs(service::start_environment(environment))
        .env_remove("OPENCODE_PASSWORD")
        .env("OPENCODE_CONFIG_DIR", string(record, "agent_home"))
        // Opening Terminal must never update an installed native agent.
        .env("OPENCODE_DISABLE_AUTOUPDATE", "true");
    if let Some(password) = &reg.password {
        result.env("OPENCODE_PASSWORD", password);
    }
    Ok(result)
}

pub(crate) fn attach(name: &str, expected_run: Option<&str>, dry: bool) -> Result<i32> {
    #[cfg(not(unix))]
    {
        let _ = (name, expected_run, dry);
        Err("Native OpenCode Terminal is not supported on this computer".into())
    }
    #[cfg(unix)]
    {
        use std::os::unix::process::CommandExt;
        let _guard = try_bindings(Duration::from_secs(2))
            .ok_or("OpenCode session is changing; try opening Terminal again")?;
        let record = binding(name)?;
        if expected_run.is_some_and(|run| record["run_id"] != run) {
            return Err(backend::RUN_CHANGED.into());
        }
        if dry {
            println!("hgs: would open the existing native OpenCode TUI for {name}");
            return Ok(0);
        }
        // Unlike a default `opencode` launch, this never discovers, starts,
        // replaces or stops a service. Its registered loopback endpoint and
        // private password go only to this explicit attach client.
        let service = running_service()?;
        verify_session(&service, &record)?;
        let mut command = command(
            &record,
            &service.reg,
            expected_run,
            std::env::vars().collect(),
        )?;
        // Recheck the binding after I/O. A rename/forget/new run cannot silently
        // redirect this client to another native conversation.
        let current = binding(name)?;
        if current["run_id"] != record["run_id"]
            || current["conversation_id"] != record["conversation_id"]
            || current["cwd"] != record["cwd"]
        {
            return Err(backend::RUN_CHANGED.into());
        }
        Err(format!(
            "Could not open native OpenCode Terminal: {}",
            command.exec()
        ))
    }
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    fn record() -> Value {
        json!({"service":"opencode2","run_id":"run-one","conversation_id":"ses_existing", "cwd":"/workspace/project", "agent_home":"/profiles/opencode"})
    }
    fn registration() -> service::Registration {
        service::Registration {
            id: "service-one".into(),
            version: "2.0.32".into(),
            port: 4096,
            pid: 42,
            password: Some("synthetic-password".into()),
        }
    }
    fn contaminated_environment() -> Vec<(String, String)> {
        vec![("OPENCODE_PASSWORD".into(), "inherited-password".into())]
    }
    #[test]
    fn terminal_uses_only_the_existing_service_and_native_conversation() {
        let cmd = command(
            &record(),
            &registration(),
            Some("run-one"),
            contaminated_environment(),
        )
        .unwrap();
        let args = cmd
            .get_args()
            .map(|a| a.to_string_lossy().into_owned())
            .collect::<Vec<_>>();
        assert_eq!(
            args,
            [
                "--server",
                "http://127.0.0.1:4096",
                "--session",
                "ses_existing"
            ]
        );
        assert_eq!(cmd.get_current_dir(), Some(Path::new("/workspace/project")));
        let env = cmd
            .get_envs()
            .map(|(k, v)| {
                (
                    k.to_string_lossy().into_owned(),
                    v.map(|v| v.to_string_lossy().into_owned()),
                )
            })
            .collect::<std::collections::BTreeMap<_, _>>();
        assert_eq!(
            env.get("OPENCODE_PASSWORD"),
            Some(&Some("synthetic-password".into()))
        );
        assert_eq!(
            env.get("OPENCODE_DISABLE_AUTOUPDATE"),
            Some(&Some("true".into()))
        );
        assert_eq!(
            env.get("OPENCODE_CONFIG_DIR"),
            Some(&Some("/profiles/opencode".into()))
        );
        assert!(!args.iter().any(|arg| arg.contains("password")
            || arg == "--prompt"
            || arg == "--auto"
            || arg == "--standalone"));
    }
    #[test]
    fn terminal_refuses_changed_unsettled_or_unscoped_sessions() {
        assert!(command(
            &record(),
            &registration(),
            Some("another-run"),
            contaminated_environment()
        )
        .is_err());
        for (key, value) in [
            ("run_id", json!("")),
            ("service", json!("opencode1")),
            ("conversation_id", json!("not-an-id")),
            ("cwd", json!("relative")),
            ("agent_home", json!("relative")),
            ("creation_pending", json!(true)),
        ] {
            let mut record = record();
            record[key] = value;
            assert!(
                command(&record, &registration(), None, contaminated_environment()).is_err(),
                "{key}"
            );
        }
        let mut reg = registration();
        reg.password = None;
        let cmd = command(&record(), &reg, None, contaminated_environment()).unwrap();
        assert!(!cmd
            .get_envs()
            .any(|(key, value)| key == "OPENCODE_PASSWORD" && value.is_some()));
    }
}
