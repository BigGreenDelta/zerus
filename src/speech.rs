//! Fixed, bounded JSON ABI to the optional local speech service. No model runtime
//! or client-supplied commands enter the CLI process.
use crate::{
    cli::{Error, Result},
    config::Config,
};
use serde_json::{json, Value};
use std::{fs, io::Read, path::PathBuf};
#[path = "speech_profiles.rs"]
mod profiles;
#[cfg(unix)]
use std::{io::Write, time::Duration};

pub const MAX_REQUEST_BYTES: usize = 5_130_000;
#[cfg(unix)]
const MAX_RESPONSE_BYTES: usize = 1_048_576;

pub fn dispatch(config: &Config, args: &[String], dry: bool) -> Result<i32> {
    if dry || args != ["--json"] {
        return Err(Error::new(
            1,
            "usage: hgs [@host] speech --json (JSON stdin)",
        ));
    }
    let mut input = Vec::new();
    std::io::stdin()
        .take((MAX_REQUEST_BYTES + 1) as u64)
        .read_to_end(&mut input)?;
    if input.len() > MAX_REQUEST_BYTES {
        return Err(Error::new(1, "Speech request is too large"));
    }
    let value: Value =
        serde_json::from_slice(&input).map_err(|_| Error::new(1, "Invalid speech JSON request"))?;
    println!("{}", request(config, &value)?);
    Ok(0)
}

fn socket_path(config: &Config) -> Result<PathBuf> {
    let settings = match fs::read(config.dir.join("speech.json")) {
        Ok(bytes) if bytes.len() <= 65_536 => serde_json::from_slice::<Value>(&bytes)
            .map_err(|_| Error::new(1, "Invalid speech host configuration"))?,
        Ok(_) => return Err(Error::new(1, "Speech configuration is too large")),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Value::Null,
        Err(e) => return Err(e.into()),
    };
    let default_state = PathBuf::from(
        std::env::var("XDG_STATE_HOME").unwrap_or_else(|_| format!("{}/.local/state", config.home)),
    )
    .join("hgs/speech");
    let state = settings
        .get("state_dir")
        .and_then(Value::as_str)
        .map(PathBuf::from)
        .unwrap_or(default_state);
    let path = settings
        .get("socket_path")
        .and_then(Value::as_str)
        .map(PathBuf::from)
        .unwrap_or_else(|| state.join("service.sock"));
    if !path.is_absolute() {
        return Err(Error::new(1, "Speech socket path must be absolute"));
    }
    Ok(path)
}

pub fn request(config: &Config, input: &Value) -> Result<Value> {
    validate_request(input).map_err(|e| Error::new(1, e.to_string()))?;
    let action = input.get("action").and_then(Value::as_str).unwrap_or("");
    if profiles::is_action(action) {
        return profiles::request(config, input)
            .map_err(|_| Error::new(1, "Speech profile management failed on this computer"));
    }
    if !matches!(
        action,
        "providers" | "submit" | "status" | "cancel" | "stats"
    ) {
        return Err(Error::new(1, "Unknown speech action"));
    }
    let bytes = serde_json::to_vec(input).map_err(|e| Error::new(1, e.to_string()))?;
    if bytes.len() > MAX_REQUEST_BYTES {
        return Err(Error::new(1, "Speech request is too large"));
    }
    let path = socket_path(config)?;
    match exchange(&path, &bytes) {
        Ok(value) => Ok(value),
        Err(error) if action == "providers" => Ok(json!({
            "schema":1,"profiles":[],"available":false,"reason":error.message
        })),
        Err(error) => Err(error),
    }
}

/// Bounded child invocation used by the durable voice worker. Input is data,
/// never shell syntax; the peer helper supervises its own SSH children.
#[cfg(unix)]
pub(crate) fn capture(
    args: &[String],
    input: &[u8],
    timeout: Duration,
) -> std::result::Result<Value, String> {
    let mut command =
        std::process::Command::new(std::env::current_exe().map_err(|e| e.to_string())?);
    command.args(args);
    capture_command(command, input, timeout)
}

#[cfg(unix)]
fn capture_command(
    mut command: std::process::Command,
    input: &[u8],
    timeout: Duration,
) -> std::result::Result<Value, String> {
    use std::{os::unix::process::CommandExt, process::Stdio, thread, time::Instant};
    command
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    // This group contains only owned CLI/SSH transport children, never the
    // resident agents reached through their existing PTY/native API.
    unsafe {
        command.pre_exec(|| {
            if libc::setsid() < 0 {
                Err(std::io::Error::last_os_error())
            } else {
                Ok(())
            }
        });
    }
    let mut child = command.spawn().map_err(|e| e.to_string())?;
    let mut stdin = child.stdin.take().ok_or("Missing child input")?;
    let mut stdout = child.stdout.take().ok_or("Missing child output")?;
    let mut stderr = child.stderr.take().ok_or("Missing child errors")?;
    let bytes = input.to_vec();
    let writer = thread::spawn(move || stdin.write_all(&bytes));
    let reader = thread::spawn(move || {
        let mut v = Vec::new();
        stdout
            .by_ref()
            .take(1_048_577)
            .read_to_end(&mut v)
            .map(|_| v)
    });
    let errors = thread::spawn(move || {
        let mut v = Vec::new();
        stderr.by_ref().take(65_537).read_to_end(&mut v).map(|_| v)
    });
    let start = Instant::now();
    let status = loop {
        match child.try_wait() {
            Ok(Some(status)) => break Some(status),
            Ok(None) if start.elapsed() < timeout => thread::sleep(Duration::from_millis(20)),
            _ => {
                unsafe {
                    libc::kill(-(child.id() as i32), libc::SIGKILL);
                }
                let _ = child.kill();
                let _ = child.wait();
                break None;
            }
        }
    };
    // A transport parent can exit while its orphaned SSH child still owns a
    // pipe. Finish the entire private group before joining pipe readers.
    unsafe {
        libc::kill(-(child.id() as i32), libc::SIGKILL);
    }
    let _ = writer.join();
    let output = reader
        .join()
        .map_err(|_| "Child output failed")?
        .map_err(|e| e.to_string())?;
    let _ = errors.join(); // Native errors can contain paths; clients get typed voice errors.
    if !status.is_some_and(|s| s.success()) {
        return Err("Speech/native command was unavailable or refused".into());
    }
    if output.len() > 1_048_576 {
        return Err("Speech/native response is too large".into());
    }
    serde_json::from_slice(&output).map_err(|_| "Invalid speech/native response".into())
}

/// Resolve only identities this host verified itself through a direct route.
#[cfg(unix)]
pub(crate) fn voice_route(
    config: &Config,
    machine_id: &str,
) -> std::result::Result<String, String> {
    let local = crate::swarm::local_node_id(config).map_err(|e| e.to_string())?;
    if machine_id == local {
        return Ok(String::new());
    }
    let mut bindings = crate::swarm::verified_bindings(config, &local);
    if let Ok(file) = fs::File::open(config.dir.join("swarm/mobile-peers.json")) {
        let mut bytes = Vec::new();
        if file.take(65_537).read_to_end(&mut bytes).is_ok() && bytes.len() <= 65_536 {
            if let Ok(value) = serde_json::from_slice::<Value>(&bytes) {
                if value["schema"] == 1 && value["local"] == local {
                    for (alias, peer) in value["peers"].as_object().into_iter().flatten() {
                        if let Some(id) = peer["machine_id"].as_str() {
                            bindings.insert(alias.clone(), id.to_owned());
                        }
                    }
                }
            }
        }
    }
    bindings
        .into_iter()
        .find(|(alias, id)| {
            id == machine_id && config.peers.contains(alias) && !config.is_self(alias)
        })
        .map(|(alias, _)| alias)
        .ok_or_else(|| {
            "The speech computer is not a verified direct peer of this conversation computer".into()
        })
}

#[cfg(unix)]
pub(crate) fn voice_request(
    config: &Config,
    machine_id: &str,
    input: &Value,
) -> std::result::Result<Value, String> {
    voice_request_timeout(config, machine_id, input, Duration::from_secs(35))
}
#[cfg(unix)]
fn voice_request_timeout(
    config: &Config,
    machine_id: &str,
    input: &Value,
    timeout: Duration,
) -> std::result::Result<Value, String> {
    validate_request(input).map_err(|e| e.to_string())?;
    let via = voice_route(config, machine_id)?;
    if via.is_empty() {
        return request(config, input).map_err(|e| e.message);
    }
    let envelope = serde_json::to_vec(&json!({"schema":1,"via":via,"target_machine_id":machine_id,"argv":["speech","--json"],"payload":input})).map_err(|e|e.to_string())?;
    capture(
        &["swarm".into(), "mobile-peer".into(), "--json".into()],
        &envelope,
        timeout,
    )
}

#[cfg(unix)]
pub(crate) fn voice_computers(config: &Config) -> std::result::Result<Value, String> {
    let deadline = std::time::Instant::now() + Duration::from_secs(25);
    // Discovery verifies aliases and refreshes the local identity cache. Its
    // transport is bounded; unavailable machines remain explicit in the result.
    let discovery = capture(
        &["swarm".into(), "mobile-peers".into(), "--json".into()],
        b"{}",
        Duration::from_secs(12),
    )?;
    let mut candidates = vec![discovery["local"].clone()];
    candidates.extend(
        discovery["peers"]
            .as_array()
            .into_iter()
            .flatten()
            .filter(|p| p["machine_id"].is_string())
            .cloned(),
    );
    let mut seen = std::collections::HashSet::new();
    candidates.retain(|p| seen.insert(p["machine_id"].clone().to_string()));
    let mut computers = Vec::new();
    std::thread::scope(|scope| {
        let (tx, rx) = std::sync::mpsc::channel();
        for worker in 0..4 {
            let candidates = &candidates;
            let tx = tx.clone();
            scope.spawn(move || {
                for computer in candidates.iter().skip(worker).step_by(4) {
                    let remaining=deadline.saturating_duration_since(std::time::Instant::now());
                    let result=if remaining<Duration::from_millis(100) {Err("Discovery deadline reached".into())} else {
                        voice_request_timeout(config,computer["machine_id"].as_str().unwrap_or(""),&json!({"action":"providers"}),remaining.min(Duration::from_secs(8)))
                    };
                    let mut row = json!({"machine_id":computer["machine_id"],"name":computer["name"],"profiles":[],"available":false});
                    match result {
                        Ok(v) if v["schema"] == 1 => { row["profiles"] = v["profiles"].clone(); row["available"] = v["available"].clone(); if v["reason"].is_string() {row["reason"] = v["reason"].clone();} }
                        _ => row["reason"] = json!("Speech is unavailable on this computer"),
                    }
                    let _ = tx.send(row);
                }
            });
        }
        drop(tx);
        computers.extend(rx);
    });
    computers.sort_by_key(|v| v["name"].as_str().unwrap_or("").to_owned());
    Ok(json!({"schema":1,"computers":computers}))
}

pub(crate) fn validate_request(input: &Value) -> anyhow::Result<()> {
    if profiles::is_action(input["action"].as_str().unwrap_or("")) {
        return profiles::validate(input);
    }
    use base64::Engine;
    let object = input
        .as_object()
        .ok_or_else(|| anyhow::anyhow!("Speech request must be an object"))?;
    let action = input.get("action").and_then(Value::as_str).unwrap_or("");
    let fields: &[&str] = match action {
        "providers" | "stats" => &["action"],
        "submit" => &["action", "request_id", "profile_id", "audio"],
        "status" | "cancel" => &["action", "job_id"],
        _ => anyhow::bail!("Unknown speech action"),
    };
    let has_revision = action == "submit" && object.contains_key("profile_revision");
    anyhow::ensure!(
        object.len() == fields.len() + usize::from(has_revision)
            && fields.iter().all(|field| object.contains_key(*field)),
        "Invalid speech request fields"
    );
    if has_revision {
        anyhow::ensure!(
            input["profile_revision"]
                .as_u64()
                .is_some_and(|r| (1..=9_007_199_254_740_991).contains(&r)),
            "Invalid speech profile revision"
        );
    }
    if action == "submit" || action == "status" || action == "cancel" {
        let key = if action == "submit" {
            "request_id"
        } else {
            "job_id"
        };
        let id = input[key].as_str().unwrap_or("");
        let uuid = uuid::Uuid::parse_str(id)
            .map_err(|_| anyhow::anyhow!("Speech job identity must be a UUID"))?;
        anyhow::ensure!(!uuid.is_nil(), "Speech job UUID must not be nil");
        anyhow::ensure!(
            uuid.hyphenated().to_string() == id,
            "Speech job UUID must be canonical lowercase"
        );
    }
    if action == "submit" {
        let profile = input["profile_id"].as_str().unwrap_or("");
        anyhow::ensure!(
            !profile.is_empty()
                && profile.len() <= 64
                && profile
                    .bytes()
                    .all(|c| c.is_ascii_alphanumeric() || c == b'-' || c == b'_'),
            "Invalid speech profile ID"
        );
        let audio = input["audio"]
            .as_object()
            .ok_or_else(|| anyhow::anyhow!("Invalid speech audio"))?;
        anyhow::ensure!(
            audio.len() == 4
                && input["audio"]["format"] == "pcm_s16le"
                && input["audio"]["sample_rate"].as_u64() == Some(16000)
                && input["audio"]["channels"].as_u64() == Some(1),
            "Audio must be mono PCM s16le at 16000 Hz"
        );
        let encoded = input["audio"]["data_base64"]
            .as_str()
            .ok_or_else(|| anyhow::anyhow!("Audio must be base64"))?;
        anyhow::ensure!(
            encoded.len() <= 5_120_000,
            "Audio exceeds the 120 second limit"
        );
        let decoded = base64::engine::general_purpose::STANDARD
            .decode(encoded)
            .map_err(|_| anyhow::anyhow!("Invalid audio base64"))?;
        anyhow::ensure!(
            !decoded.is_empty() && decoded.len() <= 3_840_000 && decoded.len() % 2 == 0,
            "Invalid PCM sample count"
        );
    }
    Ok(())
}

#[cfg(unix)]
fn exchange(path: &std::path::Path, bytes: &[u8]) -> Result<Value> {
    use std::os::unix::{
        fs::{FileTypeExt, MetadataExt},
        net::UnixStream,
    };
    let metadata = fs::symlink_metadata(path)
        .map_err(|_| Error::new(1, "Speech service is unavailable on this computer"))?;
    if !metadata.file_type().is_socket()
        || metadata.uid() != unsafe { libc::geteuid() }
        || metadata.mode() & 0o077 != 0
    {
        return Err(Error::new(
            1,
            "Speech service socket is not private to this user",
        ));
    }
    let mut stream = UnixStream::connect(path)
        .map_err(|_| Error::new(1, "Speech service is unavailable on this computer"))?;
    stream.set_read_timeout(Some(Duration::from_secs(10)))?;
    stream.set_write_timeout(Some(Duration::from_secs(10)))?;
    stream.write_all(bytes)?;
    stream.write_all(b"\n")?;
    let shutdown = stream.shutdown(std::net::Shutdown::Write);
    read_response_after_shutdown(stream, shutdown)
}

#[cfg(unix)]
fn read_response_after_shutdown(stream: impl Read, shutdown: std::io::Result<()>) -> Result<Value> {
    // A fast peer can send its reply and close before our write-half-close.
    // macOS then reports NotConnected even with a valid reply still buffered.
    // Preserve EOF framing and require the bounded, validated response below.
    match shutdown {
        Ok(()) => {}
        Err(error) if error.kind() == std::io::ErrorKind::NotConnected => {}
        Err(error) => return Err(error.into()),
    }
    let mut response = Vec::new();
    stream
        .take((MAX_RESPONSE_BYTES + 1) as u64)
        .read_to_end(&mut response)?;
    if response.len() > MAX_RESPONSE_BYTES {
        return Err(Error::new(1, "Speech service response is too large"));
    }
    let value: Value = serde_json::from_slice(&response)
        .map_err(|_| Error::new(1, "Invalid speech service response"))?;
    if value.get("schema").and_then(Value::as_u64) != Some(1) {
        return Err(Error::new(1, "Unsupported speech service schema"));
    }
    Ok(value)
}

#[cfg(not(unix))]
fn exchange(_path: &std::path::Path, _bytes: &[u8]) -> Result<Value> {
    Err(Error::new(
        1,
        "Use a configured remote computer for speech recognition",
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    #[cfg(unix)]
    fn capture_closes_owned_orphan_pipes_and_times_out() {
        let start = std::time::Instant::now();
        let mut completed = std::process::Command::new("sh");
        completed.args(["-c", "sleep 30 & printf '{\"schema\":1}'"]);
        assert_eq!(
            capture_command(completed, b"{}", Duration::from_secs(1)).unwrap()["schema"],
            1
        );
        let mut stalled = std::process::Command::new("sh");
        stalled.args(["-c", "sleep 30 & wait"]);
        assert!(capture_command(stalled, b"{}", Duration::from_millis(100)).is_err());
        assert!(start.elapsed() < Duration::from_secs(3));
    }

    #[test]
    fn rejects_unscoped_fields_bad_ids_and_audio() {
        assert!(validate_request(&json!({"action":"providers"})).is_ok());
        assert!(validate_request(&json!({"action":"stats"})).is_ok());
        assert!(validate_request(&json!({"action":"providers","command":"exec"})).is_err());
        assert!(validate_request(&json!({"action":"status","job_id":"../recording"})).is_err());
        let mut input = json!({"action":"submit","request_id":"f2934a64-a321-4c65-9a1f-54a42386ac9d","profile_id":"gigaam-ru","audio":{"format":"pcm_s16le","sample_rate":16000,"channels":1,"data_base64":"AAA="}});
        assert!(validate_request(&input).is_ok());
        input["audio"]["data_base64"] = json!("AA==");
        assert!(validate_request(&input).is_err());
        input["audio"]["data_base64"] = json!("!!!!");
        assert!(validate_request(&input).is_err());
        input["audio"]["data_base64"] = json!("AAA=");
        input["audio"]["sample_rate"] = json!(16000.0);
        assert!(validate_request(&input).is_err());
    }

    #[test]
    #[cfg(unix)]
    fn disconnected_half_close_still_requires_a_bounded_valid_response() {
        use std::io::{Cursor, Error as IoError, ErrorKind};
        let disconnected = || Err(IoError::from(ErrorKind::NotConnected));
        let expected = json!({"schema":1,"job_id":"synthetic-job","state":"queued"});
        assert_eq!(
            read_response_after_shutdown(Cursor::new(expected.to_string()), disconnected())
                .unwrap(),
            expected
        );
        for (bytes, message) in [
            (Vec::new(), "Invalid speech service response"),
            (b"not JSON\n".to_vec(), "Invalid speech service response"),
            (
                b"{\"schema\":2}\n".to_vec(),
                "Unsupported speech service schema",
            ),
            (
                b"{\"state\":\"queued\"}\n".to_vec(),
                "Unsupported speech service schema",
            ),
            (
                vec![b' '; MAX_RESPONSE_BYTES + 1],
                "Speech service response is too large",
            ),
        ] {
            assert!(
                read_response_after_shutdown(Cursor::new(bytes), disconnected())
                    .unwrap_err()
                    .message
                    .contains(message)
            );
        }
    }

    #[test]
    #[cfg(unix)]
    fn half_close_and_response_read_errors_are_not_hidden() {
        use std::io::{Error as IoError, ErrorKind};
        struct UnreadResponse;
        impl Read for UnreadResponse {
            fn read(&mut self, _: &mut [u8]) -> std::io::Result<usize> {
                panic!("A real shutdown failure must not proceed to the response");
            }
        }
        for kind in [
            ErrorKind::PermissionDenied,
            ErrorKind::BrokenPipe,
            ErrorKind::InvalidInput,
        ] {
            let error = read_response_after_shutdown(
                UnreadResponse,
                Err(IoError::new(kind, "original shutdown failure")),
            )
            .unwrap_err();
            assert_eq!(error.message, "original shutdown failure");
        }
        struct FailedResponse;
        impl Read for FailedResponse {
            fn read(&mut self, _: &mut [u8]) -> std::io::Result<usize> {
                Err(IoError::new(
                    ErrorKind::BrokenPipe,
                    "original response read failure",
                ))
            }
        }
        assert_eq!(
            read_response_after_shutdown(
                FailedResponse,
                Err(IoError::from(ErrorKind::NotConnected))
            )
            .unwrap_err()
            .message,
            "original response read failure"
        );
    }

    #[test]
    #[cfg(unix)]
    fn buffered_reply_survives_peer_close_before_half_close() {
        use std::io::{BufRead, BufReader};
        use std::os::unix::net::UnixStream;
        let (mut client, mut server) = UnixStream::pair().unwrap();
        let worker = std::thread::spawn(move || {
            let mut input = String::new();
            BufReader::new(&mut server).read_line(&mut input).unwrap();
            assert_eq!(input, "{\"action\":\"providers\"}\n");
            server
                .write_all(b"{\"schema\":1,\"profiles\":[]}\n")
                .unwrap();
        });
        client.write_all(b"{\"action\":\"providers\"}\n").unwrap();
        worker.join().unwrap(); // the peer has closed with its valid reply buffered
        let shutdown = client.shutdown(std::net::Shutdown::Write);
        let response = read_response_after_shutdown(client, shutdown).unwrap();
        assert_eq!(response, json!({"schema":1,"profiles":[]}));
    }

    #[test]
    #[cfg(unix)]
    fn socket_requires_private_permissions_and_schema() {
        use std::os::unix::{fs::PermissionsExt, net::UnixListener};
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("service.sock");
        let listener = UnixListener::bind(&path).unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o666)).unwrap();
        assert!(exchange(&path, b"{}")
            .unwrap_err()
            .message
            .contains("private"));
        fs::set_permissions(&path, fs::Permissions::from_mode(0o600)).unwrap();
        let worker = std::thread::spawn(move || {
            let (mut connection, _) = listener.accept().unwrap();
            let mut input = String::new();
            connection.read_to_string(&mut input).unwrap();
            assert_eq!(input, "{\"action\":\"providers\"}\n");
            connection.write_all(b"{\"schema\":2}\n").unwrap();
        });
        assert!(exchange(&path, b"{\"action\":\"providers\"}")
            .unwrap_err()
            .message
            .contains("schema"));
        worker.join().unwrap();
    }
}
