//! The background swarm worker as other hgs commands see it. A worker records
//! its version beside its lock and exits when an upgrade replaces its
//! executable. A worker from before that record, or from an older version,
//! cannot read catalog entries this version writes and keeps the lock while it
//! retries, so commands that read or write the catalog restart it through the
//! service manager. Only the default configuration directory is the one the
//! service uses; other directories never touch a service.
use super::{store, transport::now};
use crate::{
    config::Config,
    platform::{self, FileIdentity as Identity},
};
use anyhow::Result;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{
    fs::{self, File},
    path::{Path, PathBuf},
    process::{Command, Stdio},
    thread,
    time::{Duration, Instant},
};

const LOCK: &str = "worker.lock";
const MARKER: &str = "worker.json";
const RESTART: &str = "worker-restart.json";
/// Seconds between restart attempts, whatever their outcome.
const RESTART_INTERVAL: u64 = 600;
const VERSION: &str = env!("CARGO_PKG_VERSION");

#[derive(Debug, Serialize, Deserialize)]
struct Marker {
    version: String,
    pid: u32,
    started_at: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Status {
    pub running: bool,
    pub current: bool,
}

impl Status {
    pub fn json(self) -> Value {
        json!({"running": self.running, "current": self.current})
    }
}

/// Written by the worker once it holds the lock.
pub fn record(config: &Config) -> Result<()> {
    let marker = Marker {
        version: VERSION.into(),
        pid: std::process::id(),
        started_at: now(),
    };
    store::replace(
        &config.dir.join("swarm").join(MARKER),
        &serde_json::to_vec(&marker)?,
    )
}

/// A held lock without a live marker of this version or a newer one is stale.
fn assess(held: bool, marker: Option<&Marker>, alive: impl Fn(u32) -> bool, ours: &str) -> Status {
    Status {
        running: held,
        current: held && marker.is_some_and(|m| !older(&m.version, ours) && alive(m.pid)),
    }
}

fn older(version: &str, ours: &str) -> bool {
    fn parts(version: &str) -> Option<Vec<u64>> {
        version
            .split(['-', '+'])
            .next()?
            .split('.')
            .map(|part| part.parse().ok())
            .collect()
    }
    match (parts(version), parts(ours)) {
        (Some(theirs), Some(ours)) => theirs < ours,
        _ => version != ours,
    }
}

fn alive(pid: u32) -> bool {
    platform::process_alive(pid)
}

/// A shared probe conflicts only with the worker's exclusive lock.
fn held(dir: &Path) -> bool {
    let Ok(file) = File::open(dir.join(LOCK)) else {
        return false;
    };
    match fs2::FileExt::try_lock_shared(&file) {
        Ok(()) => {
            drop(crate::platform::FileLock::held(file));
            false
        }
        Err(error) => error.raw_os_error() == fs2::lock_contended_error().raw_os_error(),
    }
}

pub fn status(config: &Config) -> Status {
    let dir = config.dir.join("swarm");
    let held = held(&dir);
    let marker = held
        .then(|| fs::read(dir.join(MARKER)).ok())
        .flatten()
        .and_then(|bytes| serde_json::from_slice::<Marker>(&bytes).ok());
    assess(held, marker.as_ref(), alive, VERSION)
}

/// Reports the worker and restarts a stale one. Best effort: it never fails
/// the calling command.
pub fn supervise(config: &Config) -> Status {
    let status = status(config);
    if status.running && !status.current {
        let default = std::env::var_os("HGS_CONFIG_DIR").is_none();
        restart(&config.dir.join("swarm"), default, now(), || {
            if let Some((program, args)) = command() {
                run(program, &args);
            }
        });
    }
    status
}

/// One attempt per interval, recorded before it runs so concurrent commands
/// do not repeat it. Returns whether the attempt was made.
fn restart(dir: &Path, default: bool, now: u64, attempt: impl FnOnce()) -> bool {
    if !default || command().is_none() {
        return false;
    }
    let last = fs::read(dir.join(RESTART))
        .ok()
        .and_then(|bytes| serde_json::from_slice::<Value>(&bytes).ok())
        .and_then(|value| value["attempted_at"].as_u64());
    if !due(last, now)
        || store::replace(
            &dir.join(RESTART),
            json!({"attempted_at": now}).to_string().as_bytes(),
        )
        .is_err()
    {
        return false;
    }
    attempt();
    true
}

/// A clock that moved backwards does not postpone the next attempt indefinitely.
fn due(last: Option<u64>, now: u64) -> bool {
    last.is_none_or(|last| now.abs_diff(last) >= RESTART_INTERVAL)
}

/// Restarts only the swarm worker unit; agents, tmux servers, DeepSeek hosts
/// and hgs-recovery run elsewhere.
#[cfg(target_os = "linux")]
fn command() -> Option<(&'static str, Vec<String>)> {
    Some((
        "systemctl",
        ["--user", "--no-block", "try-restart", "hgs-swarm.service"]
            .map(String::from)
            .to_vec(),
    ))
}
#[cfg(target_os = "macos")]
fn command() -> Option<(&'static str, Vec<String>)> {
    let uid = unsafe { libc::getuid() };
    Some((
        "launchctl",
        vec![
            "kickstart".into(),
            "-k".into(),
            format!("gui/{uid}/com.hgdev.hgs-swarm"),
        ],
    ))
}
/// No service manager runs the worker here: the Windows desktop app supervises it.
#[cfg(not(any(target_os = "linux", target_os = "macos")))]
fn command() -> Option<(&'static str, Vec<String>)> {
    None
}

/// Waits briefly; a hung service manager never holds up the command.
fn run(program: &str, args: &[String]) {
    let Ok(mut child) = crate::platform::spawn(
        Command::new(program)
            .args(args)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null()),
    ) else {
        return;
    };
    let started = Instant::now();
    while matches!(child.try_wait(), Ok(None)) {
        if started.elapsed() >= Duration::from_secs(5) {
            let _ = child.kill();
            let _ = child.wait();
            return;
        }
        thread::sleep(Duration::from_millis(25));
    }
}

fn identity(path: &Path) -> Option<Identity> {
    platform::file_identity(path)
}

/// The file this process was started from.
pub struct Executable {
    path: PathBuf,
    started: Identity,
}

impl Executable {
    pub fn current() -> Option<Self> {
        let path = std::env::current_exe().ok()?;
        // The running image rather than its path, which an upgrade may already
        // have replaced. Linux reaches a replaced image through /proc and names
        // it "<path> (deleted)". Where the image cannot be read (Windows, or a
        // macOS query that fails), the file at the path stands in for it as
        // before, and a replacement made while this runs goes unnoticed.
        let started = platform::image_identity().or_else(|| identity(&path))?;
        let path = path
            .to_str()
            .and_then(|path| path.strip_suffix(" (deleted)"))
            .map_or(path.clone(), PathBuf::from);
        Some(Self { path, started })
    }
    /// A different file at the same path. A missing file may be mid-upgrade.
    pub fn replaced(&self) -> bool {
        identity(&self.path).is_some_and(|now| now != self.started)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{cell::Cell, collections::BTreeMap};

    fn marker(version: &str, pid: u32) -> Marker {
        Marker {
            version: version.into(),
            pid,
            started_at: 1,
        }
    }

    #[test]
    fn stale_workers_are_missing_older_or_dead() {
        let alive = |pid: u32| pid == 42;
        let current = Status {
            running: true,
            current: true,
        };
        let stale = Status {
            running: true,
            current: false,
        };
        assert_eq!(assess(true, None, alive, "1.47.0"), stale);
        assert_eq!(
            assess(true, Some(&marker("1.46.1", 42)), alive, "1.47.0"),
            stale
        );
        assert_eq!(
            assess(true, Some(&marker("1.9.0", 42)), alive, "1.10.0"),
            stale
        );
        assert_eq!(
            assess(true, Some(&marker("garbage", 42)), alive, "1.47.0"),
            stale
        );
        assert_eq!(
            assess(true, Some(&marker("1.47.0", 7)), alive, "1.47.0"),
            stale
        );
        assert_eq!(
            assess(true, Some(&marker("1.47.0", 42)), alive, "1.47.0"),
            current
        );
        // A newer worker parses everything this version writes.
        assert_eq!(
            assess(true, Some(&marker("1.48.0", 42)), alive, "1.47.0"),
            current
        );
        let absent = Status {
            running: false,
            current: false,
        };
        assert_eq!(
            assess(false, Some(&marker("1.47.0", 42)), alive, "1.47.0"),
            absent
        );
        assert!(super::alive(std::process::id()));
        assert!(!super::alive(0));
        assert!(!super::alive(u32::MAX));
    }

    #[test]
    fn restarts_are_rate_limited_and_never_leave_an_override_directory() {
        assert!(due(None, 1_000));
        assert!(!due(Some(1_000), 1_599));
        assert!(due(Some(1_000), 1_600));
        assert!(due(Some(10_000), 1_000)); // the clock moved back
        let dir = tempfile::tempdir().unwrap();
        let attempts = Cell::new(0);
        let attempt = || attempts.set(attempts.get() + 1);
        assert!(!restart(dir.path(), false, 1_000, attempt));
        assert_eq!(attempts.get(), 0);
        assert!(!dir.path().join(RESTART).exists());
        let supported = command().is_some();
        assert_eq!(restart(dir.path(), true, 1_000, attempt), supported);
        assert!(!restart(dir.path(), true, 1_300, attempt));
        assert_eq!(restart(dir.path(), true, 1_600, attempt), supported);
        assert_eq!(attempts.get(), if supported { 2 } else { 0 });
    }

    #[test]
    fn only_the_swarm_worker_unit_is_restarted() {
        match command() {
            Some(("systemctl", args)) => assert_eq!(
                args,
                ["--user", "--no-block", "try-restart", "hgs-swarm.service"]
            ),
            Some(("launchctl", args)) => {
                assert_eq!(args[..2], ["kickstart", "-k"]);
                assert!(args[2].starts_with("gui/") && args[2].ends_with("/com.hgdev.hgs-swarm"));
            }
            other => assert!(other.is_none()),
        }
    }

    #[test]
    fn status_reads_the_lock_and_marker() {
        let dir = tempfile::tempdir().unwrap();
        let config = Config {
            home: dir.path().to_str().unwrap().into(),
            dir: dir.path().join("config"),
            state: dir.path().join("state"),
            selves: vec![],
            peers: vec![],
            base_peers: vec![],
            peers_from_environment: false,
            machines: BTreeMap::new(),
            tab: false,
            tab_colors: String::new(),
        };
        let swarm = config.dir.join("swarm");
        assert_eq!(status(&config), assess(false, None, alive, VERSION));
        fs::create_dir_all(&swarm).unwrap();
        let lock = File::create(swarm.join(LOCK)).unwrap();
        assert!(!status(&config).running);
        fs2::FileExt::try_lock_exclusive(&lock).unwrap();
        assert_eq!(
            status(&config),
            Status {
                running: true,
                current: false
            }
        );
        record(&config).unwrap();
        assert_eq!(
            status(&config),
            Status {
                running: true,
                current: true
            }
        );
        let written: Marker =
            serde_json::from_slice(&fs::read(swarm.join(MARKER)).unwrap()).unwrap();
        assert_eq!(
            (written.version.as_str(), written.pid),
            (VERSION, std::process::id())
        );
        // The probe never takes the worker's place.
        fs2::FileExt::unlock(&lock).unwrap();
        assert!(!status(&config).running);
        fs2::FileExt::try_lock_exclusive(&lock).unwrap();
    }

    #[test]
    fn a_replaced_executable_is_detected() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("hgs");
        fs::write(&path, b"old").unwrap();
        let executable = Executable {
            path: path.clone(),
            started: identity(&path).unwrap(),
        };
        assert!(!executable.replaced());
        // Package managers install the new file under a temporary name and rename it.
        let next = dir.path().join(".hgs.new");
        fs::write(&next, b"new").unwrap();
        fs::remove_file(&path).unwrap();
        assert!(!executable.replaced()); // mid-upgrade
        fs::rename(&next, &path).unwrap();
        assert!(executable.replaced());
        // An in-place rewrite changes the modification time.
        let executable = Executable {
            path: path.clone(),
            started: Identity {
                modified: 0,
                ..identity(&path).unwrap()
            },
        };
        assert!(executable.replaced());
        assert!(Executable::current().is_some_and(|current| !current.replaced()));
    }

    /// Set, to the folder they share, for the process
    /// `a_replacement_while_a_worker_starts_is_detected` starts.
    #[cfg(unix)]
    const PROBE: &str = "HGS_TEST_REPLACED_EXECUTABLE";
    /// How long that test and its probe each wait for the other.
    #[cfg(unix)]
    const PROBE_LIMIT: Duration = Duration::from_secs(60);

    /// Run by `a_replacement_while_a_worker_starts_is_detected`: reads its
    /// identity only after another file took the path it was started from.
    #[cfg(unix)]
    #[test]
    #[ignore = "run by a_replacement_while_a_worker_starts_is_detected"]
    fn replaced_while_starting_probe() {
        let Some(dir) = std::env::var_os(PROBE).map(PathBuf::from) else {
            return;
        };
        fs::write(dir.join("started"), b"").unwrap();
        // It also ends by itself if the test never answers.
        let deadline = Instant::now() + PROBE_LIMIT;
        while !dir.join("replaced").exists() {
            assert!(Instant::now() < deadline, "the executable was not replaced");
            thread::sleep(Duration::from_millis(10));
        }
        let executable = Executable::current().unwrap();
        println!("probe replaced={}", executable.replaced());
    }

    /// The probe process, which only this test starts and reaps, and the folder
    /// with its executable link, log and markers. The folder is removed only once
    /// the probe is known to be reaped; otherwise it is kept as evidence.
    #[cfg(unix)]
    struct Probe {
        child: std::process::Child,
        /// Its start as `platform::process_identity` read it before anything
        /// reaped the child, so the ID could not name another process.
        birth: Option<String>,
        path: PathBuf,
        dir: tempfile::TempDir,
        status: Option<std::process::ExitStatus>,
        stopped: Option<Result<std::process::ExitStatus, String>>,
    }

    #[cfg(unix)]
    impl Probe {
        fn new(child: std::process::Child, path: PathBuf, dir: tempfile::TempDir) -> Self {
            let birth = platform::process_identity(child.id())
                .filter(|process| process.parent == std::process::id())
                .map(|process| process.birth);
            Self {
                child,
                birth,
                path,
                dir,
                status: None,
                stopped: None,
            }
        }
        fn describe(&self) -> String {
            let (pid, birth) = (self.child.id(), &self.birth);
            format!("probe {pid} (birth {birth:?}, {})", self.path.display())
        }
        /// Its exit status once reaped. Only this method reaps it.
        fn poll(&mut self) -> std::io::Result<Option<std::process::ExitStatus>> {
            if self.status.is_none() {
                self.status = self.child.try_wait()?;
            }
            Ok(self.status)
        }
        /// Kills the probe if it still runs and reaps it, waiting at most 10 s
        /// after the kill. An error says why it may still exist.
        fn stop(&mut self) -> Result<std::process::ExitStatus, String> {
            if let Some(result) = &self.stopped {
                return result.clone();
            }
            let mut killed = None;
            let deadline = Instant::now() + Duration::from_secs(10);
            let result = loop {
                match self.poll() {
                    Ok(Some(status)) => break Ok(status),
                    // Not reaped, so the ID is still this child's.
                    Ok(None) if killed.is_none() => killed = Some(self.child.kill()),
                    Ok(None) if Instant::now() < deadline => {
                        thread::sleep(Duration::from_millis(10))
                    }
                    Ok(None) => break Err(format!("not reaped 10 s after SIGKILL ({killed:?})")),
                    Err(error) => break Err(format!("unknown status: {error} (kill {killed:?})")),
                }
            };
            // Once reaped, no process with this ID and start may remain.
            let result =
                result.and_then(|status| match platform::process_identity(self.child.id()) {
                    Some(process) if Some(&process.birth) == self.birth.as_ref() => {
                        Err(format!("still running after it was reaped with {status}"))
                    }
                    _ => Ok(status),
                });
            self.stopped = Some(result.clone());
            result
        }
    }

    #[cfg(unix)]
    impl Drop for Probe {
        /// Also runs while a failed assertion unwinds, so nothing here panics.
        fn drop(&mut self) {
            use std::io::Write;
            let explicit = self.stopped.is_some();
            let result = self.stop();
            // A probe that may still exist keeps its folder, and only that one.
            self.dir.disable_cleanup(result.is_err());
            if !explicit || result.is_err() {
                let folder = if result.is_err() { "kept" } else { "removed" };
                let (probe, dir) = (self.describe(), self.dir.path().display());
                let _ = writeln!(std::io::stderr(), "{probe}: {result:?}; {folder} {dir}");
            }
        }
    }

    #[cfg(unix)]
    #[test]
    fn a_replacement_while_a_worker_starts_is_detected() {
        use std::io::Read;
        let test = std::env::current_exe().unwrap();
        // A link, not a copy: a file this process just wrote can be busy for exec.
        let dir = tempfile::tempdir_in(test.parent().unwrap()).unwrap();
        let path = dir.path().join("hgs");
        let (started, replaced) = (dir.path().join("started"), dir.path().join("replaced"));
        let next = dir.path().join(".hgs.new");
        fs::hard_link(&test, &path).unwrap();
        // Output goes to a file, so neither process can block on a pipe.
        let log = dir.path().join("probe.log");
        let output = File::create(&log).unwrap();
        let printed = || {
            let mut bytes = Vec::new();
            if let Ok(file) = File::open(&log) {
                let _ = file.take(64 * 1024).read_to_end(&mut bytes);
            }
            String::from_utf8_lossy(&bytes).into_owned()
        };
        let child = Command::new(&path)
            .args([
                "swarm::service::tests::replaced_while_starting_probe",
                "--exact",
                "--ignored",
                "--test-threads=1",
                "--nocapture",
            ])
            .env(PROBE, dir.path())
            .stdin(Stdio::null())
            .stdout(output.try_clone().unwrap())
            .stderr(output)
            .spawn()
            .unwrap();
        let mut probe = Probe::new(child, path.clone(), dir);
        let deadline = Instant::now() + PROBE_LIMIT;
        while !started.exists() {
            let running = probe.poll().unwrap().is_none();
            assert!(running && Instant::now() < deadline, "{}", printed());
            thread::sleep(Duration::from_millis(10));
        }
        // As an upgrade does between exec and the worker's first look.
        fs::write(&next, b"new").unwrap();
        fs::rename(&next, &path).unwrap();
        fs::write(&replaced, b"").unwrap();
        let deadline = Instant::now() + PROBE_LIMIT;
        while probe.poll().unwrap().is_none() {
            assert!(Instant::now() < deadline, "{}", printed());
            thread::sleep(Duration::from_millis(10));
        }
        // The cleanup result: reaped, with no process left under its ID and start.
        let status = probe.stop();
        let ended = status.as_ref().is_ok_and(|status| status.success());
        assert!(ended, "{}: {status:?}\n{}", probe.describe(), printed());
        assert!(printed().contains("probe replaced=true"), "{}", printed());
    }
}
