//! Keep process-exit evidence separate from provider SessionEnd notifications.
use super::*;
use std::os::unix::process::ExitStatusExt;
use std::process::{Child, Command, ExitStatus};
use std::sync::atomic::{AtomicI32, Ordering};

static CHILD: AtomicI32 = AtomicI32::new(0);
static SHUTDOWN_SIGNAL: AtomicI32 = AtomicI32::new(0);

pub(super) fn shutdown_requested() -> bool {
    SHUTDOWN_SIGNAL.load(Ordering::SeqCst) != 0
}

extern "C" fn signal_handler(signal: i32) {
    // Ctrl-C is an agent interaction, often just interrupting its current turn.
    // A handler (not SIG_IGN) resets to default in exec'd children.
    if signal == libc::SIGINT {
        return;
    }
    SHUTDOWN_SIGNAL.store(signal, Ordering::SeqCst);
    let pid = CHILD.load(Ordering::SeqCst);
    if pid > 0 {
        unsafe {
            libc::kill(pid, signal);
        }
    }
}

pub(super) struct Signals(Vec<(i32, libc::sigaction)>);
impl Signals {
    pub(super) fn install() -> Result<Self> {
        SHUTDOWN_SIGNAL.store(0, Ordering::SeqCst);
        let mut guard = Self(Vec::new());
        for signal in [libc::SIGINT, libc::SIGTERM, libc::SIGHUP] {
            let mut action: libc::sigaction = unsafe { std::mem::zeroed() };
            let mut previous: libc::sigaction = unsafe { std::mem::zeroed() };
            action.sa_sigaction = signal_handler as *const () as libc::sighandler_t;
            action.sa_flags = libc::SA_RESTART;
            unsafe {
                libc::sigemptyset(&mut action.sa_mask);
            }
            if unsafe { libc::sigaction(signal, &action, &mut previous) } != 0 {
                return Err(io::Error::last_os_error().to_string());
            }
            guard.0.push((signal, previous));
        }
        Ok(guard)
    }
}
impl Drop for Signals {
    fn drop(&mut self) {
        CHILD.store(0, Ordering::SeqCst);
        for (signal, previous) in &self.0 {
            unsafe {
                libc::sigaction(*signal, previous, std::ptr::null_mut());
            }
        }
    }
}

pub(super) fn spawn(argv: &[String], name: &str, run_id: &str) -> Result<Child> {
    let _guard = lock(None)?;
    let mut record = read_run(name, run_id)?.ok_or("session changed while launching agent")?;
    let child = Command::new(&argv[0])
        .args(&argv[1..])
        .spawn()
        .map_err(|error| format!("{}: {error}", argv[0]))?;
    CHILD.store(child.id() as i32, Ordering::SeqCst);
    let signal = SHUTDOWN_SIGNAL.load(Ordering::SeqCst);
    if signal != 0 {
        unsafe {
            libc::kill(child.id() as i32, signal);
        }
    }
    record["pid"] = json!(child.id());
    record["process_start"] = json!(process_start(child.id()));
    record["supervisor"] =
        json!({"pid": std::process::id(), "start": process_start(std::process::id())});
    write(&mut record)?;
    Ok(child)
}

pub(super) fn wait(child: &mut Child) -> Result<ExitStatus> {
    let status = child.wait().map_err(|error| error.to_string())?;
    CHILD.store(0, Ordering::SeqCst);
    Ok(status)
}

pub(super) fn finish(name: &str, run_id: &str, child_id: u32, status: ExitStatus) -> Result<()> {
    // A clean exit is the user quitting the agent. Hooks share the state lock,
    // so Overmind hears of the end after it.
    if let Some(archived) = record_exit(name, run_id, child_id, status)? {
        crate::overmind::session_ended(&archived);
    }
    Ok(())
}

/// The session's current name when the exit archived it; it may have been
/// renamed since launch.
fn record_exit(name: &str, run_id: &str, child_id: u32, status: ExitStatus) -> Result<Option<String>> {
    let _guard = lock(None)?;
    let Some(mut record) = read_run(name, run_id)? else {
        return Ok(None);
    };
    if record["pid"].as_u64() != Some(child_id.into()) {
        return Ok(None);
    }
    let signal = SHUTDOWN_SIGNAL.load(Ordering::SeqCst);
    record["exit_code"] = json!(status.code());
    record["termination_signal"] =
        json!(status
            .signal()
            .or(if signal == 0 { None } else { Some(signal) }));
    record["exited_at"] = json!(now());
    record["completion_source"] = json!("supervisor");
    let paused = record.get("pausing").is_some() || record["paused"].as_bool().unwrap_or(false);
    let clean = status.success()
        && signal == 0
        && !paused
        && string(&record, "expected_id").is_empty()
        && !string(&record, "conversation_id").is_empty()
        && string(&record, "error").is_empty()
        && archive::known_exit(&record);
    record["completion_reason"] = json!(if paused {
        "pause"
    } else if signal != 0 || status.signal().is_some() {
        "signal"
    } else if clean {
        "clean_exit"
    } else {
        "unconfirmed_exit"
    });
    write(&mut record)?;
    if archive::failed_restore(&record)? {
        return Ok(None);
    }
    if !clean {
        return Ok(None);
    }
    archive::save(&record)?;
    Ok(Some(string(&record, "name").to_owned()))
}
