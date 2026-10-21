use super::*;
use fs2::FileExt;
use sha2::{Digest, Sha256};
use std::fs::{self, OpenOptions};
use std::io::Write;

pub(super) fn root() -> PathBuf {
    nonempty_env("HGS_STATE_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            nonempty_env("XDG_STATE_HOME")
                .map(PathBuf::from)
                .unwrap_or_else(|| home().join(".local/state"))
                .join("hgs/sessions")
        })
}

pub(super) fn private_dir(path: &Path) -> Result<()> {
    crate::platform::private_dir(path).map_err(|e| e.to_string())
}

pub(super) fn absolute_root() -> Result<PathBuf> {
    private_dir(&root())?;
    crate::platform::canonical(root()).map_err(|e| e.to_string())
}

pub(super) fn atomic(path: &Path, contents: &str) -> Result<()> {
    let parent = path.parent().ok_or("cannot write file without parent")?;
    private_dir(parent)?;
    let mut temp = tempfile::Builder::new()
        .prefix(".hgs-")
        .tempfile_in(parent)
        .map_err(|e| e.to_string())?;
    temp.write_all(contents.as_bytes())
        .map_err(|e| e.to_string())?;
    temp.as_file().sync_all().map_err(|e| e.to_string())?;
    crate::platform::persist(temp, path).map_err(|e| e.to_string())?;
    crate::platform::sync_dir(parent).map_err(|e| e.to_string())
}

pub(super) type Guard = crate::platform::FileLock;

pub(super) fn lock(path: Option<&Path>) -> Result<Guard> {
    let default = root().join(".lock");
    let path = path.unwrap_or(&default);
    private_dir(path.parent().ok_or("invalid lock path")?)?;
    let file = lock_file(path)?;
    let guard = Guard::exclusive(file).map_err(|e| e.to_string())?;
    if path == default {
        rename::recover_locked()?;
        rename::sync_legacy_locked()?;
    }
    Ok(guard)
}

/// An exclusive lock on an auxiliary file, or None when another process still
/// holds it at `deadline`. Use writer_until for the default state lock so that
/// its recovery protocol runs as well.
pub(super) fn lock_until(path: &Path, deadline: std::time::Instant) -> Result<Option<Guard>> {
    private_dir(path.parent().ok_or("invalid lock path")?)?;
    let file = lock_file(path)?;
    loop {
        match file.try_lock_exclusive() {
            Ok(()) => return Ok(Some(Guard::held(file))),
            Err(error) if !crate::platform::lock_contended(&error) => return Err(error.to_string()),
            Err(_) if std::time::Instant::now() >= deadline => return Ok(None),
            Err(_) => std::thread::sleep(std::time::Duration::from_millis(5)),
        }
    }
}

/// The normal writer-lock recovery, with a bounded admission wait for optional
/// lifecycle cleanup. Never turn a busy writer into an unbounded archive exit.
pub(super) fn writer_until(deadline: std::time::Instant) -> Result<Option<Guard>> {
    private_dir(&root())?;
    let Some(guard) = lock_until(&root().join(".lock"), deadline)? else { return Ok(None); };
    rename::recover_locked()?;
    rename::sync_legacy_locked()?;
    Ok(Some(guard))
}

/// An owner-only lock file, never written. Windows locks a byte range only
/// through a handle with read or write access, which append-only lacks.
pub(super) fn lock_file(path: &Path) -> Result<fs::File> {
    crate::platform::private_open(OpenOptions::new().create(true).truncate(false).write(true))
        .open(path)
        .map_err(|e| e.to_string())
}

pub(super) fn legacy_record_path(name: &str) -> PathBuf {
    root().join(format!("{:x}.json", Sha256::digest(name.as_bytes())))
}

pub(super) fn route_path(name: &str) -> PathBuf {
    root()
        .join("name_routes")
        .join(legacy_record_path(name).file_name().unwrap())
}

pub(super) fn record_path(name: &str) -> PathBuf {
    let legacy = legacy_record_path(name);
    if route_path(name).exists() {
        root().join("bindings").join(legacy.file_name().unwrap())
    } else {
        legacy
    }
}

pub(super) fn read(name: &str) -> Result<Value> {
    let contents = fs::read_to_string(record_path(name)).map_err(|e| {
        if e.kind() == io::ErrorKind::NotFound {
            format!("{name}: no saved binding; restart through hgs to enable tracking")
        } else {
            e.to_string()
        }
    })?;
    let mut record: Value = serde_json::from_str(&contents)
        .map_err(|e| format!("{name}: invalid saved binding: {e}"))?;
    if !record.is_object()
        || record["version"] != 1
        || string(&record, "name") != name
        || !AGENTS.contains(&string(&record, "agent"))
    {
        return Err(format!("{name}: invalid saved binding"));
    }
    journal::repair_kimi_main(&mut record);
    journal::repair_claude_internal(&mut record);
    claude_subagents::reconcile(&mut record);
    native_subagents::reconcile(&mut record);
    Ok(record)
}

pub(super) fn write(record: &mut Value) -> Result<()> {
    record["updated"] = json!(now());
    atomic(&record_path(string(record, "name")), &format!("{record}\n"))?;
    rename::mirror_legacy(record)
}

/// Names are mutable; provider hooks and already-running supervisors retain
/// their original environment. The run token is the immutable binding identity.
pub(super) fn read_run(name: &str, run_id: &str) -> Result<Option<Value>> {
    if run_id.is_empty() {
        return Ok(None);
    }
    if record_path(name).exists() {
        let record = read(name)?;
        if string(&record, "run_id") == run_id {
            return Ok(Some(record));
        }
    }
    let mut matching = records()?
        .into_iter()
        .filter(|record| string(record, "run_id") == run_id);
    let record = matching.next();
    if matching.next().is_some() {
        return Err("ambiguous session run identity".into());
    }
    Ok(record)
}

/// Bindings are named by `legacy_record_path`; other JSON files in the state
/// root, such as desktop presence and read marks, are not session records.
fn is_record_path(path: &Path) -> bool {
    let stem = path.file_stem().and_then(|stem| stem.to_str()).unwrap_or_default();
    path.extension().and_then(|extension| extension.to_str()) == Some("json")
        && stem.len() == 64
        && stem.bytes().all(|byte| matches!(byte, b'0'..=b'9' | b'a'..=b'f'))
}

pub(super) fn records() -> Result<Vec<Value>> {
    if !root().exists() {
        return Ok(Vec::new());
    }
    let mut paths: Vec<PathBuf> = fs::read_dir(root())
        .map_err(|e| e.to_string())?
        .filter_map(|entry| entry.ok().map(|entry| entry.path()))
        .filter(|path| is_record_path(path))
        .collect();
    let bindings = root().join("bindings");
    if bindings.exists() {
        paths.extend(
            fs::read_dir(bindings)
                .map_err(|e| e.to_string())?
                .filter_map(|entry| entry.ok().map(|entry| entry.path()))
                .filter(|path| is_record_path(path)),
        );
    }
    paths.sort();
    let mut result = Vec::new();
    for path in paths {
        let loaded = (|| {
            let value: Value =
                serde_json::from_str(&fs::read_to_string(&path).map_err(|e| e.to_string())?)
                    .map_err(|e| e.to_string())?;
            if value["rename_shadow"] == true {
                return Ok(None);
            }
            read(string(&value, "name")).map(Some)
        })();
        match loaded {
            Ok(Some(record)) => result.push(record),
            Ok(None) => {}
            Err(error) => eprintln!("hgs: cannot read {}: {error}", path.display()),
        }
    }
    Ok(result)
}

pub(super) type Pane = (String, String, String);
pub(super) type Snapshot = BTreeMap<String, Vec<Pane>>;

/// Each live session's panes as (pane, dead, run marker). No server means none.
pub(super) fn live() -> Result<Snapshot> {
    live_sessions().map(|(snapshot, _)| snapshot)
}

/// `live`, and from the same answer each live session's name by its immutable
/// session ID (`$N`), which a rename keeps.
pub(super) fn live_sessions() -> Result<(Snapshot, BTreeMap<String, String>)> {
    let mut result: Snapshot = BTreeMap::new();
    let mut names = BTreeMap::new();
    for row in backend::current().panes()? {
        if !row.session_id.is_empty() {
            names.insert(row.session_id, row.session.clone());
        }
        result
            .entry(row.session)
            .or_default()
            .push((row.pane, row.dead, row.run_marker));
    }
    Ok((result, names))
}

pub(super) fn matches(record: &Value, panes: Option<&Vec<Pane>>) -> bool {
    panes
        .map(|panes| {
            panes.len() == 1
                && panes[0].0 == string(record, "pane")
                && panes[0].1 == "0"
                && panes[0].2 == string(record, "run_id")
                && !panes[0].2.is_empty()
        })
        .unwrap_or(false)
}

pub(super) fn process_start(pid: u32) -> String {
    crate::platform::process_start_time(pid)
        .unwrap_or_else(|| crate::platform::process_start_fallback(pid).unwrap_or_default())
}

pub(super) fn process_alive(record: &Value) -> bool {
    let start = string(record, "process_start");
    let pid = record["pid"]
        .as_u64()
        .and_then(|pid| u32::try_from(pid).ok())
        .unwrap_or(0);
    !start.is_empty() && pid > 0 && process_start(pid) == start
}

pub(super) fn pause_active(record: &Value) -> bool {
    let owner = &record["pausing"];
    let pid = owner["pid"]
        .as_u64()
        .and_then(|pid| u32::try_from(pid).ok())
        .unwrap_or(0);
    !string(owner, "start").is_empty() && process_start(pid) == string(owner, "start")
}

pub(super) fn join_command(args: &[String]) -> String {
    args.iter()
        .map(|arg| shell_words::quote(arg).into_owned())
        .collect::<Vec<_>>()
        .join(" ")
}

/// The hgs that a session's supervisor, hooks and recipes run: on Windows only
/// the stable CLI, whichever hgs.exe runs now (spec D8).
#[cfg(windows)]
pub(super) fn runner_executable() -> Result<String> {
    crate::platform::stable_executable().map(|path| path.display().to_string())
}

#[cfg(unix)]
pub(super) fn runner_executable() -> Result<String> {
    if let Some(executable) = nonempty_env("HGS_EXECUTABLE") {
        let path = Path::new(&executable);
        if path.is_absolute() && crate::platform::executable(path) {
            return Ok(executable);
        }
    }
    Ok(std::env::current_exe()
        .map_err(|error| error.to_string())?
        .to_string_lossy()
        .into_owned())
}

/// The pane command of a tracked session started from its saved `record`
/// (resume, restore, fresh start, fork): `executable --run 0 ARGV`, through the
/// record's login shell with the account's variables on Unix, directly on
/// Windows, where the session's own variables carry the account
/// (`account_variables`, spec D8).
pub(super) fn supervisor_command(
    record: &Value,
    executable: &str,
    argv: Vec<String>,
    hint_environment: &[String],
) -> Vec<String> {
    #[cfg(unix)]
    let mut launch = {
        let shell = if string(record, "shell").is_empty() {
            "/bin/bash"
        } else {
            string(record, "shell")
        };
        vec![
            shell.into(),
            "-lic".into(),
            crate::accounts::run_script(
                string(record, "account_id"),
                string(record, "agent"),
                string(record, "agent_home"),
                hint_environment,
            ),
            executable.into(),
            "0".into(),
        ]
    };
    #[cfg(windows)]
    let mut launch = {
        let _ = (record, hint_environment);
        vec![executable.into(), "--run".into(), "0".into()]
    };
    launch.extend(argv);
    launch
}

/// The account variables of a Windows session started from `record`, after its
/// other variables so that they win; none on Unix, where `supervisor_command`
/// applies them after the login shell.
pub(super) fn account_variables(record: &Value) -> Vec<String> {
    if cfg!(windows) {
        crate::accounts::run_environment(
            string(record, "account_id"),
            string(record, "agent"),
            string(record, "agent_home"),
        )
    } else {
        Vec::new()
    }
}

#[cfg(test)]
mod agent_hint_tests {
    use super::*;

    #[test]
    fn agent_hint_transport_keeps_project_launchers_user_flags_and_native_argv() {
        let hint = vec!["HGS_AGENT_HINT=1".into(), "HGS_AGENT_HINT_OFF=".into()];
        for (agent, argv) in [
            (
                "claude",
                vec![
                    "project-claude",
                    "--resume",
                    "conversation",
                    "--append-system-prompt",
                    "user append",
                    "--system-prompt",
                    "user system",
                    "--settings",
                    "user.json",
                ],
            ),
            (
                "codex",
                vec![
                    "project-codex",
                    "resume",
                    "conversation",
                    "-c",
                    "developer_instructions=\"User instructions\"",
                    "--profile",
                    "work",
                ],
            ),
        ] {
            let record = json!({"agent":agent,"account_id":"work-account","agent_home":"/private/profile","shell":"/private/login-shell"});
            let native = argv.into_iter().map(str::to_owned).collect::<Vec<_>>();
            let command = supervisor_command(&record, "/stable/hgs", native.clone(), &hint);
            assert_eq!(&command[command.len() - native.len()..], native.as_slice());
            #[cfg(unix)]
            {
                assert_eq!(&command[..2], &["/private/login-shell", "-lic"]);
                let script = shell_words::split(&command[2]).unwrap();
                assert!(script.contains(&"HGS_AGENT_HINT=1".into()));
                assert!(script.contains(&"HGS_AGENT_HINT_OFF=".into()));
                assert!(!script
                    .iter()
                    .any(|word| word.starts_with("HGS_CONFIG_DIR=")));
                assert_eq!(&command[3..5], &["/stable/hgs", "0"]);
            }
            #[cfg(windows)]
            assert_eq!(&command[..3], &["/stable/hgs", "--run", "0"]);
        }
    }
}
