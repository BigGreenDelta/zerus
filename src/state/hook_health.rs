//! Content-free hook registration and observed-callback evidence. Reads never
//! execute a provider, contact a service, scan a journal or change native trust.
use super::*;
use crate::{cli, config::Config};
use sha2::{Digest, Sha256};
use std::fs::{self, OpenOptions};

const CONFIG_MAX: usize = 1024 * 1024;
const REQUEST_MAX: usize = 4096;
const UNKNOWN: &str = "Native policy, trust and the configuration loaded by a running agent are not verified by this file inspection.";

pub(super) fn read_bounded(path: &Path, maximum: usize) -> Result<Option<Vec<u8>>> {
    match fs::symlink_metadata(path) {
        Ok(metadata) if !metadata.is_file() => {
            return Err("Hook evidence is not a regular file".into())
        }
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(_) => return Err("Hook evidence is unreadable".into()),
        _ => {}
    }
    let mut options = OpenOptions::new();
    options.read(true);
    // no_follow also opens nonblocking on Unix: a FIFO swap cannot hang.
    let file = match crate::platform::no_follow(&mut options).open(path) {
        Ok(file) => file,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(_) => return Err("Hook evidence is unreadable".into()),
    };
    let metadata = file.metadata().map_err(|_| "Hook evidence is unreadable")?;
    if !metadata.is_file() || metadata.len() > maximum as u64 {
        return Err("Hook evidence is oversized or not a regular file".into());
    }
    let mut bytes = Vec::new();
    file.take(maximum as u64 + 1)
        .read_to_end(&mut bytes)
        .map_err(|_| "Hook evidence is unreadable")?;
    if bytes.len() > maximum {
        return Err("Hook evidence is oversized".into());
    }
    Ok(Some(bytes))
}

fn digest(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}
fn config_file(directory: &Path, provider: &str) -> Result<PathBuf> {
    Ok(directory.join(match provider {
        "claude" => "settings.json",
        "codex" => "hooks.json",
        "kimi" => "config.toml",
        _ => return Err("This provider uses a native API, not tracking hook configuration".into()),
    }))
}

/// Bind the requested path and every traversed symlink as well as its resolved
/// target. No private path or hook command is returned to the caller.
fn path_identity(path: &Path) -> Result<(PathBuf, String)> {
    if !path.is_absolute() {
        return Err("Hook profile path is not absolute".into());
    }
    let resolved = hooks::resolved_target(path).map_err(|_| "Cannot resolve hook configuration")?;
    let mut evidence = Vec::new();
    for path in [path, resolved.as_path()] {
        for prefix in path.ancestors() {
            let metadata = fs::symlink_metadata(prefix);
            let identity = match metadata {
                Ok(metadata) => {
                    #[cfg(unix)]
                    let id = {
                        use std::os::unix::fs::MetadataExt;
                        format!("{}:{}", metadata.dev(), metadata.ino())
                    };
                    #[cfg(windows)]
                    let id = format!("{:?}", metadata.created().ok());
                    json!([
                        prefix,
                        metadata.is_dir(),
                        metadata.file_type().is_symlink(),
                        id,
                        fs::read_link(prefix).ok()
                    ])
                }
                Err(error) if error.kind() == io::ErrorKind::NotFound => json!([prefix, "missing"]),
                Err(_) => return Err("Cannot inspect hook configuration identity".into()),
            };
            evidence.push(identity);
        }
    }
    Ok((resolved, digest(json!(evidence).to_string().as_bytes())))
}

struct Snapshot {
    path: PathBuf,
    identity: String,
    bytes: Option<Vec<u8>>,
    report: Value,
    known: Option<(u64, u64, u64)>,
}
fn snapshot(directory: &Path, provider: &str, program: &str) -> Result<Snapshot> {
    let requested = config_file(directory, provider)?;
    let (path, identity) = path_identity(&requested)?;
    let known = hooks::health_version(&path, program);
    let bytes = read_bounded(&path, CONFIG_MAX)?;
    let mut report = match std::str::from_utf8(bytes.as_deref().unwrap_or_default()) {
        Ok(text) => hooks::health_document(text, provider, known),
        Err(_) => Err("Invalid hook configuration".into()),
    }
    .unwrap_or_else(|_| json!({"state":"invalid","compatibility":{"state":"unknown"}}));
    if bytes.is_none() {
        report["state"] = json!("missing");
    }
    report["sha256"] = json!(digest(bytes.as_deref().unwrap_or_default()));
    report["path_id"] = json!(identity);
    Ok(Snapshot {
        path,
        identity,
        bytes,
        report,
        known,
    })
}

fn report(
    provider: &str,
    account: &str,
    directory: &Path,
    program: &str,
    record: Option<&Value>,
) -> Value {
    let configuration = if AGENTS.contains(&provider) {
        snapshot(directory, provider, program)
            .map(|snapshot| snapshot.report)
            .unwrap_or_else(|_| json!({"state":"unreadable","compatibility":{"state":"unknown"}}))
    } else {
        json!({"state":"not_applicable","compatibility":{"state":"not_applicable"}})
    };
    json!({"schema":1,"provider":provider,"account_id":account,"configuration":configuration,
        "effective":{"state":"unknown","reason":UNKNOWN},"trust":{"state":"unknown","reason":"Only an explicit native trust decision can grant trust; hook registration is not trust."},
        "observed":record.map(observed).unwrap_or_else(||json!({"state":"unknown","events":[]})),
        "running_agents_retrofitted":false})
}

/// Bounded per-run counters; no prompt, paths, tool inputs or payload content.
/// Called only after the hook receiver has checked native run/conversation.
pub(super) fn observe(record: &mut Value, event: &Value) {
    if !string(event, "agent_id").is_empty() {
        return;
    }
    let kind = string(event, "hook_event_name");
    let allowed = hooks::health_events(string(record, "agent"));
    if !allowed.contains(&kind) {
        return;
    }
    let old = &record["hook_observation"];
    let mut events = serde_json::Map::new();
    if old["run_id"] == record["run_id"] && old["conversation_id"] == record["conversation_id"] {
        for name in &allowed {
            let value = &old["events"][name];
            if let (Some(count), Some(at)) = (
                value["count"].as_u64(),
                value["last_at"]
                    .as_f64()
                    .filter(|n| n.is_finite() && *n > 0.),
            ) {
                events.insert((*name).into(), json!({"count":count,"last_at":at}));
            }
        }
    }
    let mut evidence = json!({"run_id":record["run_id"],"conversation_id":record["conversation_id"],"events":events});
    let count = evidence["events"][kind]["count"]
        .as_u64()
        .unwrap_or(0)
        .saturating_add(1);
    evidence["events"][kind] = json!({"count":count,"last_at":now()});
    record["hook_observation"] = evidence;
}
fn observed(record: &Value) -> Value {
    let evidence = &record["hook_observation"];
    if evidence["run_id"] != record["run_id"]
        || evidence["conversation_id"] != record["conversation_id"]
    {
        return json!({"state":"unknown","events":[]});
    }
    let events: Vec<Value> = hooks::health_events(string(record, "agent"))
        .iter()
        .filter_map(|kind| {
            let value = &evidence["events"][kind];
            let count = value["count"].as_u64().filter(|n| *n > 0)?;
            let at = value["last_at"]
                .as_f64()
                .filter(|n| n.is_finite() && *n > 0.)?;
            Some(json!({"event":kind,"count":count,"last_at":at}))
        })
        .collect();
    json!({"state":if events.is_empty(){"unknown"}else{"observed"},"events":events})
}
fn bounded<'a>(value: &'a Value, key: &str, maximum: usize, empty: bool) -> Result<&'a str> {
    value[key]
        .as_str()
        .filter(|text| {
            (empty || !text.is_empty())
                && text.len() <= maximum
                && !text.chars().any(char::is_control)
        })
        .ok_or_else(|| format!("Invalid hook health {key}"))
}
fn request() -> Result<Value> {
    let mut bytes = Vec::new();
    io::stdin()
        .take(REQUEST_MAX as u64 + 1)
        .read_to_end(&mut bytes)
        .map_err(|_| "Cannot read hook health request")?;
    if bytes.len() > REQUEST_MAX {
        return Err("Hook health request is too large".into());
    }
    serde_json::from_slice(&bytes).map_err(|_| "Invalid hook health request".into())
}
fn exact_keys(input: &Value, fields: &[&str]) -> Result<()> {
    if input.as_object().is_none_or(|object| {
        object.len() != fields.len() || object.keys().any(|key| !fields.contains(&key.as_str()))
    }) {
        return Err("Invalid hook health request fields".into());
    }
    Ok(())
}
fn request_id(input: &Value) -> Result<&str> {
    let id = bounded(input, "request_id", 36, false)?;
    if uuid::Uuid::parse_str(id)
        .ok()
        .is_none_or(|uuid| uuid.to_string() != id)
    {
        return Err("Invalid hook health request_id".into());
    }
    Ok(id)
}
fn session_report(name: &str, input: &Value) -> Result<Value> {
    exact_keys(
        input,
        &["request_id", "expected_run_id", "expected_conversation_id"],
    )?;
    let id = request_id(input)?;
    bounded(input, "expected_run_id", 128, false)?;
    bounded(input, "expected_conversation_id", 256, false)?;
    // Read a single immutable-target record; never use inspection/reconciliation
    // helpers that can contact a provider or scan native history.
    let path = if opencode::exists(name) {
        root()
            .join("opencode/bindings")
            .join(format!("{}.json", digest(name.as_bytes())))
    } else if dsh::exists(name) {
        root()
            .join("dsh/bindings")
            .join(format!("{}.json", digest(name.as_bytes())))
    } else {
        record_path(name)
    };
    let bytes = read_bounded(&path, CONFIG_MAX)?.ok_or("No saved hook health binding")?;
    let record: Value =
        serde_json::from_slice(&bytes).map_err(|_| "Invalid hook health binding")?;
    if record["name"] != name
        || record["run_id"] != input["expected_run_id"]
        || record["conversation_id"] != input["expected_conversation_id"]
    {
        return Err("Hook health session identity changed".into());
    }
    let provider = string(&record, "agent");
    if !AGENTS.contains(&provider) && !["opencode", "dsh"].contains(&provider) {
        return Err("Unsupported hook health provider".into());
    }
    if AGENTS.contains(&provider) && record["version"] != 1 {
        return Err("Unsupported hook health binding version".into());
    }
    if string(&record, "account_id")
        .strip_prefix("native-")
        .is_some_and(|native| native != provider)
    {
        return Err("Hook health account identity changed".into());
    }
    let account = if string(&record, "account_id").is_empty() {
        format!("native-{provider}")
    } else {
        string(&record, "account_id").to_owned()
    };
    let directory = if string(&record, "agent_home").is_empty() {
        home().join(if provider == "kimi" {
            ".kimi-code".into()
        } else {
            format!(".{provider}")
        })
    } else {
        PathBuf::from(string(&record, "agent_home"))
    };
    let program = record["base"]
        .as_array()
        .and_then(|base| base.first())
        .and_then(Value::as_str)
        .unwrap_or("");
    let mut value = report(provider, &account, &directory, program, Some(&record));
    value["request_id"] = json!(id);
    value["name"] = json!(name);
    value["run_id"] = record["run_id"].clone();
    value["conversation_id"] = record["conversation_id"].clone();
    Ok(value)
}
fn repair(config: &Config, account: &str, input: &Value) -> Result<Value> {
    exact_keys(
        input,
        &[
            "request_id",
            "expected_provider",
            "expected_account_id",
            "expected_config_sha256",
            "expected_path_id",
        ],
    )?;
    let id = request_id(input)?;
    let (provider, directory) = crate::accounts::hook_profile(config, account)
        .map_err(|_| "Hook profile is unavailable")?;
    if input["expected_account_id"] != account || input["expected_provider"] != provider {
        return Err("Hook repair account identity changed".into());
    }
    for key in ["expected_config_sha256", "expected_path_id"] {
        let text = bounded(input, key, 64, false)?;
        if text.len() != 64
            || !text
                .bytes()
                .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
        {
            return Err("Invalid hook repair fingerprint".into());
        }
    }
    let requested = config_file(&directory, &provider)?;
    let before = snapshot(&directory, &provider, &provider)?;
    if before.identity != input["expected_path_id"]
        || before.report["sha256"] != input["expected_config_sha256"]
    {
        return Err("Hook configuration changed; inspect it again before repair".into());
    }
    if before.report["state"] == "invalid" {
        return Err("Invalid hook configuration; nothing repaired".into());
    }
    if before.known.is_none() {
        return Err(
            "Native compatibility is unknown; explicit repair is unavailable for this profile"
                .into(),
        );
    }
    let lock_path = before.path.with_file_name(format!(
        "{}.hgs.lock",
        before
            .path
            .file_name()
            .ok_or("Invalid hook file")?
            .to_string_lossy()
    ));
    let _guard = lock(Some(&lock_path))?;
    let (current_provider, current_directory) = crate::accounts::hook_profile(config, account)
        .map_err(|_| "Hook profile is unavailable")?;
    if current_provider != provider || current_directory != directory {
        return Err("Hook repair account identity changed".into());
    }
    let current = snapshot(&directory, &provider, &provider)?;
    if current.identity != input["expected_path_id"]
        || current.report["sha256"] != input["expected_config_sha256"]
        || current.path != before.path
    {
        return Err("Hook configuration changed; inspect it again before repair".into());
    }
    if current.report["state"] == "invalid" {
        return Err("Invalid hook configuration; nothing repaired".into());
    }
    let known = current.known.ok_or(
        "Native compatibility is unknown; explicit repair is unavailable for this profile",
    )?;
    let original = std::str::from_utf8(current.bytes.as_deref().unwrap_or_default())
        .map_err(|_| "Invalid hook configuration")?;
    let merged = hooks::repair_document(original, &current.path, &provider, known)?;
    if let Some(content) = &merged {
        // A second bounded read catches a non-cooperating editor before commit.
        let (path, identity) = path_identity(&requested)?;
        if path != current.path
            || identity != current.identity
            || read_bounded(&path, CONFIG_MAX)? != current.bytes
        {
            return Err("Hook configuration changed before repair".into());
        }
        let backup = path.with_file_name(format!(
            "{}.pre-hgs",
            path.file_name().unwrap().to_string_lossy()
        ));
        if !original.is_empty()
            && fs::symlink_metadata(&backup).is_err_and(|e| e.kind() == io::ErrorKind::NotFound)
        {
            atomic(&backup, original)?;
        }
        atomic(&path, content)?;
    }
    let mut result = report(&provider, account, &directory, &provider, None);
    result["request_id"] = json!(id);
    result["status"] = json!(if merged.is_some() {
        "repaired"
    } else {
        "unchanged"
    });
    result["notice"]=json!("Only configuration was repaired. Running agents were not restarted or retrofitted; native trust still requires an explicit decision.");
    Ok(result)
}

pub(crate) fn dispatch(config: &Config, args: &[String], dry: bool) -> cli::Result<i32> {
    let result = (|| -> Result<Value> {
        if dry {
            return Err("Hook health does not support --dry-run".into());
        }
        match args {
            [flag, account, json] if flag == "--account" && json == "--json" => {
                let (provider, directory) = crate::accounts::hook_profile(config, account)
                    .map_err(|_| "Hook profile is unavailable")?;
                Ok(report(&provider, account, &directory, &provider, None))
            }
            [flag, account, repair_flag, json]
                if flag == "--account" && repair_flag == "--repair" && json == "--json" =>
            {
                repair(config, account, &request()?)
            }
            [name, json] if json == "--json" && !name.starts_with('-') && name.len() <= 512 => {
                session_report(name, &request()?)
            }
            _ => Err(
                "usage: hgs hooks <session> --json | hgs hooks --account ID [--repair] --json"
                    .into(),
            ),
        }
    })()
    .map_err(|error| cli::Error::new(1, error))?;
    println!("{result}");
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn observation_is_bounded_content_free_and_never_crosses_conversation() {
        let mut record = json!({"agent":"claude","run_id":"run-1","conversation_id":"one"});
        let event = json!({"hook_event_name":"Stop","prompt":"private payload","tool_input":{"secret":"never copy"}});
        observe(&mut record, &event);
        observe(&mut record, &event);
        assert_eq!(observed(&record)["events"][0]["count"], 2);
        assert!(!record["hook_observation"].to_string().contains("private"));
        observe(
            &mut record,
            &json!({"hook_event_name":"arbitrary-private-name"}),
        );
        observe(
            &mut record,
            &json!({"hook_event_name":"Stop","agent_id":"child"}),
        );
        assert_eq!(observed(&record)["events"].as_array().unwrap().len(), 1);
        assert_eq!(observed(&record)["events"][0]["count"], 2);
        record["conversation_id"] = json!("two");
        assert_eq!(observed(&record)["state"], "unknown");
        observe(&mut record, &event);
        assert_eq!(observed(&record)["events"][0]["count"], 1);
    }
    #[test]
    fn silence_or_other_run_is_unknown_not_missing() {
        for evidence in [
            Value::Null,
            json!({"run_id":"old","conversation_id":"one","events":{"Stop":{"count":1,"last_at":1.}}}),
        ] {
            let record = json!({"agent":"claude","run_id":"new","conversation_id":"one","hook_observation":evidence});
            assert_eq!(observed(&record), json!({"state":"unknown","events":[]}));
        }
    }
    #[test]
    fn oversized_or_special_files_are_refused_before_reading() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("config");
        fs::write(&path, b"12345").unwrap();
        assert!(read_bounded(&path, 4).is_err());
        assert!(read_bounded(temp.path(), 4).is_err());
        assert_eq!(read_bounded(&temp.path().join("absent"), 4).unwrap(), None);
    }
    #[cfg(unix)]
    #[test]
    fn a_named_pipe_is_refused_without_waiting_for_a_writer() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("fifo");
        let name = std::ffi::CString::new(path.as_os_str().as_encoded_bytes()).unwrap();
        assert_eq!(unsafe { libc::mkfifo(name.as_ptr(), 0o600) }, 0);
        let started = std::time::Instant::now();
        assert!(read_bounded(&path, 64).is_err());
        assert!(started.elapsed() < std::time::Duration::from_secs(1));
    }
    #[cfg(unix)]
    #[test]
    fn path_identity_binds_symlink_and_replacement() {
        use std::os::unix::fs::symlink;
        let temp = tempfile::tempdir().unwrap();
        let a = temp.path().join("a");
        let b = temp.path().join("b");
        let link = temp.path().join("link");
        fs::write(&a, b"same").unwrap();
        fs::write(&b, b"same").unwrap();
        symlink(&a, &link).unwrap();
        let old = path_identity(&link).unwrap();
        fs::remove_file(&link).unwrap();
        symlink(&b, &link).unwrap();
        assert_ne!(old, path_identity(&link).unwrap());
    }
}
