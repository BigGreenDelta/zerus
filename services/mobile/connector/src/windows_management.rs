//! Explicit per-user lifecycle. No service registration and no PID/tree kill.
//! Both management and startup serialize on a private configuration lock.
use crate::{config, fsguard, journal, native, runtime};
use serde_json::{json, Value};
use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

pub type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;
const CONTROL: &str = "connector-control.json";
const STOP: &str = "connector-stop";
const LIMIT: u64 = 65_536;
fn refuse<T>(message: &str) -> Result<T> {
    Err(io::Error::other(message).into())
}
fn nonce() -> Result<String> {
    use ring::rand::{SecureRandom, SystemRandom};
    let mut bytes = [0u8; 16];
    SystemRandom::new()
        .fill(&mut bytes)
        .map_err(|_| io::Error::other("random identity unavailable"))?;
    Ok(bytes.iter().map(|b| format!("{b:02x}")).collect())
}
fn read(path: &Path) -> Result<Option<Vec<u8>>> {
    let file = match zerus_windows_private::open_file(path, false, false) {
        Ok(file) => file,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(e) => return Err(e.into()),
    };
    let mut bytes = Vec::new();
    file.take(LIMIT + 1).read_to_end(&mut bytes)?;
    if bytes.len() as u64 > LIMIT {
        return refuse("private configuration exceeds its limit");
    }
    Ok(Some(bytes))
}
fn value(bytes: Option<&[u8]>) -> Result<Value> {
    match bytes {
        Some(b) => Ok(serde_json::from_slice(b)?),
        None => Ok(json!({})),
    }
}
fn revision(bytes: Option<&[u8]>) -> String {
    bytes.map(zerus_relay::json::digest).unwrap_or_default()
}
fn write(file: &mut File, bytes: &[u8]) -> Result<()> {
    file.seek(SeekFrom::Start(0))?;
    file.set_len(0)?;
    file.write_all(bytes)?;
    file.sync_all()?;
    Ok(())
}
pub struct Guard {
    _directory: zerus_windows_private::DirectoryGuard,
    _lock: File,
}
fn try_guard(path: &Path, create: bool) -> Result<Option<Guard>> {
    let parent = path.parent().ok_or("configuration parent required")?;
    let directory = zerus_windows_private::directory(parent, create)?;
    let lock =
        zerus_windows_private::open_file(&parent.join("gateway-management.lock"), true, true)?;
    match lock.try_lock() {
        Ok(()) => {}
        Err(std::fs::TryLockError::WouldBlock) => return Ok(None),
        Err(std::fs::TryLockError::Error(error)) => return Err(error.into()),
    }
    Ok(Some(Guard {
        _directory: directory,
        _lock: lock,
    }))
}
pub fn guard(path: &Path, create: bool) -> Result<Guard> {
    try_guard(path, create)?.ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::WouldBlock,
            "another connector management operation is in progress",
        )
        .into()
    })
}
/// The parent's readiness probe can briefly own the same lock. Retry only
/// contention for one allowance, without restarting the child or changing the
/// expected revision. This bounds retries, not an OS/filesystem stall.
pub fn admission_guard(path: &Path) -> Result<Guard> {
    let deadline = Instant::now() + crate::admission::ALLOWANCE;
    crate::admission::acquire(
        || try_guard(path, false),
        || deadline.saturating_duration_since(Instant::now()),
        std::thread::sleep,
    )?
    .ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::TimedOut,
            "connector admission lock remained busy",
        )
        .into()
    })
}
fn validated(path: &Path) -> Result<config::Config> {
    let (text, _) = config::read_file(path, 0)?;
    Ok(config::validate(
        &config::parse(&text)?,
        false,
        &config::Environment::current()?,
    )?)
}
fn status(path: &Path) -> Result<Value> {
    let bytes = read(path)?;
    let settings = value(bytes.as_deref())?;
    let config = if bytes.is_some() {
        Some(config::validate(
            &config::parse(&serde_json::to_string(&settings)?)?,
            false,
            &config::Environment::current()?,
        )?)
    } else {
        None
    };
    let _state_guard = config
        .as_ref()
        .filter(|c| c.state_dir.exists())
        .map(|c| zerus_windows_private::directory(&c.state_dir, false))
        .transpose()?;
    let lock = config
        .as_ref()
        .map(|c| fsguard::probe_lock(&c.state_dir.join(journal::LOCK)));
    let running = lock == Some(fsguard::LockState::Held);
    if lock == Some(fsguard::LockState::Unknown) {
        return refuse("connector ownership could not be verified");
    }
    let control = if running {
        value(read(&config.as_ref().unwrap().state_dir.join(CONTROL))?.as_deref())?
    } else {
        json!({})
    };
    if running && control["revision"] != revision(bytes.as_deref()) {
        return refuse("running connector configuration identity changed");
    }
    Ok(
        json!({"configured":bytes.is_some(), "server_url":settings.get("server_url"),
        "state_dir":settings.get("state_dir"), "revision":revision(bytes.as_deref()),
        "running":running, "generation":control.get("generation"),
        "startup_at_login":false}),
    )
}

/// Secrets are supplied only over stdin. Responses never contain a token.
pub fn manage(action: &str, path: &Path, input: &Value) -> Result<Value> {
    let fields = input
        .as_object()
        .ok_or("management input must be an object")?;
    let allowed: &[&str] = match action {
        "status" => &["revision"],
        "save" => &["revision", "server_url", "node_token"],
        "start" => &["revision"],
        "stop" => &["revision", "generation"],
        _ => &[],
    };
    if fields
        .iter()
        .any(|(key, value)| !allowed.contains(&key.as_str()) || !value.is_string())
    {
        return refuse("invalid management fields");
    }
    if !["status", "save", "start", "stop"].contains(&action) {
        return refuse("unknown connector management action");
    }
    if action == "status" && !path.parent().is_some_and(Path::exists) {
        return Ok(
            json!({"configured":false,"running":false,"revision":"","startup_at_login":false}),
        );
    }
    if action == "status" {
        let _directory = zerus_windows_private::directory(
            path.parent().ok_or("configuration parent required")?,
            false,
        )?;
        return status(path);
    }
    let _guard = guard(path, action == "save")?;
    let current = status(path)?;
    if input.get("revision") != current.get("revision") {
        return refuse("configuration changed; reload before this action");
    }
    if action == "save" {
        if current["running"] == true {
            return refuse("stop the connector and drain its current request before saving");
        }
        let bytes = read(path)?;
        let mut settings = value(bytes.as_deref())?;
        let server = input["server_url"].as_str().ok_or("HTTPS relay required")?;
        if !server.starts_with("https://") {
            return refuse("HTTPS relay required");
        }
        let replacement = input["node_token"].as_str().unwrap_or("");
        let token = if replacement.is_empty() {
            settings["node_token"].as_str().unwrap_or("")
        } else {
            replacement
        };
        if bytes.is_some()
            && settings["server_url"] != server
            && (replacement.is_empty() || settings["node_token"] == replacement)
        {
            return refuse("a different relay requires a new computer credential");
        }
        let changed =
            bytes.is_none() || settings["server_url"] != server || settings["node_token"] != token;
        let token = token.to_owned();
        settings["server_url"] = json!(server);
        settings["node_token"] = json!(token);
        if changed {
            settings
                .as_object_mut()
                .ok_or("configuration object required")?
                .remove("identity_url");
            settings["state_dir"] = json!(path
                .parent()
                .unwrap()
                .join(format!("receipts-{}", nonce()?)));
        }
        // Always the executable shipped beside this connector, never PATH or
        // a GUI-supplied command. Native stdout/stdin stay on the hgs adapter.
        settings["hgs_path"] = json!(std::env::current_exe()?.parent().unwrap().join("hgs.exe"));
        settings["poll_interval"] = json!(5);
        let text = serde_json::to_string(&settings)?;
        let config = config::validate(
            &config::parse(&text)?,
            false,
            &config::Environment::current()?,
        )?;
        if changed {
            zerus_windows_private::directory(&config.state_dir, true)?;
        }
        let temporary = path.with_file_name(format!(".connector-{}.tmp", nonce()?));
        let result = (|| -> Result<()> {
            let mut file = zerus_windows_private::open_file(&temporary, true, true)?;
            write(&mut file, text.as_bytes())?;
            drop(file);
            // MoveFileExW with replacement and write-through, no remove-then-
            // rename window and no partial credential file on interruption.
            if read(path)? != bytes {
                return refuse("configuration changed while saving; reload before retrying");
            }
            atomic_replace(&temporary, path)?;
            Ok(())
        })();
        if result.is_err() {
            let _ = std::fs::remove_file(&temporary);
        }
        result?;
        return status(path);
    }
    if current["configured"] != true {
        return refuse("save a connector configuration first");
    }
    let config = validated(path)?;
    if action == "stop" {
        if current["running"] != true {
            return Ok(current);
        }
        let generation = current["generation"]
            .as_str()
            .filter(|v| v.len() == 32)
            .ok_or("connector generation unavailable")?;
        if input["generation"] != generation {
            return refuse("connector generation changed; reload before stopping");
        }
        let mut file = zerus_windows_private::open_file(&config.state_dir.join(STOP), true, true)?;
        write(&mut file, generation.as_bytes())?;
        return Ok(json!({"stopping":true,"generation":generation}));
    }
    if current["running"] == true {
        return Ok(current);
    }
    let executable = std::env::current_exe()?;
    let expected_revision = current["revision"]
        .as_str()
        .ok_or("configuration revision unavailable")?;
    let removed_keys: Vec<_> = native::CONTEXT_KEYS
        .into_iter()
        .chain(["HGS_HOST_SESSION", "HGS_HOST_PANE"])
        .collect();
    // The GUI's job permits breakaway. Refuse launch if an enclosing job does
    // not permit it; never silently attach the connector to a kill-on-close job.
    // The child has a nonblocking admission guard. Release before spawning so
    // it cannot race this parent's still-held lock. A Save in this gap changes
    // the revision and makes child admission refuse before any runtime exists.
    drop(_guard);
    // Null standard streams alone still let Rust's broad inheritance copy the
    // manager's original pipe handles. The fixed launcher allowlists NUL only.
    let child = zerus_windows_private::spawn_connector(
        &executable,
        expected_revision,
        path,
        &removed_keys,
    )?;
    // Readiness requires the observed revision, journal lock and generation.
    let deadline = Instant::now() + Duration::from_secs(8);
    loop {
        if let Ok(_ready_guard) = guard(path, false) {
            if let Ok(state) = status(path) {
                if state["running"] == true
                    && state["revision"] == current["revision"]
                    && state["generation"].as_str().is_some_and(|g| g.len() == 32)
                {
                    return Ok(state);
                }
            }
        }
        // A concurrent Start of the same revision may have admitted first.
        // Its confirmed readiness above remains idempotent success.
        if child.try_wait()?.is_some() {
            return refuse("connector could not start; run its private --check report");
        }
        if Instant::now() >= deadline {
            return refuse(
                "connector start was not confirmed; reload status without retrying start",
            );
        }
        std::thread::sleep(Duration::from_millis(50));
    }
}
fn atomic_replace(from: &Path, to: &Path) -> io::Result<()> {
    zerus_windows_private::replace(from, to)
}

pub fn prepare_running(state: &Path, revision: &str) -> Result<String> {
    let generation = nonce()?;
    let mut stop = zerus_windows_private::open_file(&state.join(STOP), true, true)?;
    write(&mut stop, b"")?;
    let mut control = zerus_windows_private::open_file(&state.join(CONTROL), true, true)?;
    write(
        &mut control,
        serde_json::to_string(
            &json!({"generation":generation,"revision":revision,"pid":std::process::id()}),
        )?
        .as_bytes(),
    )?;
    Ok(generation)
}
pub async fn signals(stopping: &runtime::Event, state: PathBuf, generation: String) {
    let stop_file = async {
        loop {
            if read(&state.join(STOP))
                .ok()
                .flatten()
                .is_some_and(|b| b == generation.as_bytes())
            {
                break;
            }
            tokio::time::sleep(Duration::from_millis(200)).await;
        }
    };
    let console = async {
        if tokio::signal::ctrl_c().await.is_err() {
            std::future::pending::<()>().await;
        }
    };
    futures_util::future::select(std::pin::pin!(stop_file), std::pin::pin!(console)).await;
    runtime::log("INFO", "Connector stopping after the request in progress");
    stopping.set();
    std::future::pending::<()>().await;
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::{Cell, RefCell};

    struct Scratch(PathBuf);
    impl Scratch {
        fn new() -> Self {
            let path = std::env::temp_dir().join(format!("gateway-admission-{}", nonce().unwrap()));
            zerus_windows_private::directory(&path, true).unwrap();
            Self(path)
        }
    }
    impl Drop for Scratch {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn held_management_guard_releases_the_same_child_admission() {
        let scratch = Scratch::new();
        let path = scratch.0.join("mobile/connector.json");
        let held = RefCell::new(Some(guard(&path, true).unwrap()));
        let elapsed = Cell::new(Duration::ZERO);
        let attempts = Cell::new(0);
        let acquired = crate::admission::acquire(
            || {
                attempts.set(attempts.get() + 1);
                try_guard(&path, false)
            },
            || crate::admission::ALLOWANCE.saturating_sub(elapsed.get()),
            |delay| {
                // Reaching the wait proves the real OS lock reported contention.
                assert_eq!(attempts.get(), 1);
                drop(held.borrow_mut().take());
                elapsed.set(elapsed.get() + delay);
            },
        )
        .unwrap();
        assert!(acquired.is_some());
        assert_eq!(attempts.get(), 2);
        assert!(!path.exists());
        drop(acquired);
    }

    #[test]
    fn held_management_guard_exhausts_only_one_child_allowance() {
        let scratch = Scratch::new();
        let path = scratch.0.join("mobile/connector.json");
        let held = guard(&path, true).unwrap();
        let elapsed = Cell::new(Duration::ZERO);
        let acquired = crate::admission::acquire(
            || try_guard(&path, false),
            || crate::admission::ALLOWANCE.saturating_sub(elapsed.get()),
            |delay| elapsed.set(elapsed.get() + delay),
        )
        .unwrap();
        assert!(acquired.is_none());
        assert_eq!(elapsed.get(), crate::admission::ALLOWANCE);
        assert!(!path.exists());
        drop(held);
    }
}
