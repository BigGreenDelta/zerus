//! A background worker that a service manager keeps alive. launchd (KeepAlive)
//! and systemd (Restart=always) start it again a few seconds after it exits,
//! so the service's own worker must not exit only because another worker,
//! such as one a client started detached, holds the executor lock: it would
//! be started again every few seconds, forever. It waits for the lock instead,
//! holding a standby lock meanwhile, and any other worker gives way to it
//! between rounds, so the service ends up owning the work.
use super::{lock_file, Result};
use crate::platform::FileLock;
use fs2::FileExt;
use std::{
    path::{Path, PathBuf},
    time::Duration,
};

/// How a worker is known to its service manager.
pub(super) struct Service {
    /// The systemd user unit (Linux).
    #[cfg_attr(not(target_os = "linux"), allow(dead_code))]
    pub unit: &'static str,
    /// The launchd job label (macOS).
    #[cfg_attr(not(target_os = "macos"), allow(dead_code))]
    pub label: &'static str,
}

impl Service {
    /// Whether this process is the service's own worker.
    pub fn is_current(&self) -> bool {
        current(self)
    }
}

/// The executor lock, and the standby lock when the service's worker waited
/// for it.
pub(super) struct Owner {
    _lock: FileLock,
    standby: Option<FileLock>,
    standby_path: PathBuf,
}

impl Owner {
    /// A worker that did not wait as the service's worker gives way to one
    /// that waits now.
    pub fn yields(&self) -> bool {
        self.standby.is_none() && waiting(&self.standby_path)
    }
}

/// Whether the service's worker waits for the executor lock.
pub(super) fn waiting(standby: &Path) -> bool {
    crate::status::locked(standby)
}

fn take(path: &Path) -> Result<Option<FileLock>> {
    let file = lock_file(path)?;
    Ok(file
        .try_lock_exclusive()
        .is_ok()
        .then(|| FileLock::held(file)))
}

/// Takes the executor lock, or returns `None` when this worker should exit:
/// the service's worker waits for the lock, or another worker holds it and
/// this one is not the service's. The service's worker waits instead, polling
/// every `poll`, until it holds the lock or its executable is `replaced`.
/// `is_service` runs only when another worker holds the lock.
pub(super) fn acquire(
    lock_path: &Path,
    standby_path: &Path,
    is_service: impl FnOnce() -> bool,
    replaced: impl Fn() -> bool,
    poll: Duration,
) -> Result<Option<Owner>> {
    // Only the service's worker takes the standby lock: whoever holds it is
    // that worker, and the executor lock is left to it.
    if waiting(standby_path) {
        return Ok(None);
    }
    let lock = lock_file(lock_path)?;
    let owner = |lock, standby| Owner {
        _lock: FileLock::held(lock),
        standby,
        standby_path: standby_path.to_path_buf(),
    };
    if lock.try_lock_exclusive().is_ok() {
        return Ok(Some(owner(lock, None)));
    }
    if !is_service() {
        return Ok(None);
    }
    eprintln!(
        "hgs: another worker holds {}; waiting to take over",
        lock_path.display()
    );
    let mut standby = None;
    loop {
        // A worker probing the standby lock at this instant only delays it;
        // without it the worker still waits, and is not started again.
        if standby.is_none() {
            standby = take(standby_path).unwrap_or(None);
        }
        if replaced() {
            return Ok(None);
        }
        std::thread::sleep(poll);
        if lock.try_lock_exclusive().is_ok() {
            return Ok(Some(owner(lock, standby)));
        }
    }
}

/// The systemd unit whose control group holds this process, from
/// /proc/self/cgroup: the unified hierarchy (`0::/...`) or the `name=systemd`
/// one.
#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
fn systemd_unit(cgroup: &str) -> Option<&str> {
    cgroup.lines().find_map(|line| {
        let mut fields = line.splitn(3, ':');
        let (_, controllers, path) = (fields.next()?, fields.next()?, fields.next()?);
        if !(controllers.is_empty() || controllers == "name=systemd") {
            return None;
        }
        path.rsplit('/').next().filter(|unit| !unit.is_empty())
    })
}

/// The job's own process ID from `launchctl print`, one level inside its
/// braces, as the desktop's single-instance check reads it.
#[cfg_attr(not(target_os = "macos"), allow(dead_code))]
fn launchd_pid(description: &str) -> Option<u32> {
    let mut depth = 0;
    let mut pid = None;
    for raw in description.lines() {
        let line = raw.trim();
        if line == "}" {
            depth -= 1;
            continue;
        }
        if depth == 1 {
            if let Some(value) = line.strip_prefix("pid = ") {
                pid = value.trim().parse().ok().filter(|pid| *pid > 0);
            }
        }
        if line.ends_with('{') {
            depth += 1;
        }
    }
    pid
}

/// systemd keeps a unit's processes in its control group.
#[cfg(target_os = "linux")]
fn current(service: &Service) -> bool {
    std::fs::read_to_string("/proc/self/cgroup")
        .is_ok_and(|cgroup| systemd_unit(&cgroup) == Some(service.unit))
}
/// launchd reports the job's process; a worker a client started detached has
/// the same parent and may have inherited the job's environment.
#[cfg(target_os = "macos")]
fn current(service: &Service) -> bool {
    use std::{process::Command, time::Instant};
    let uid = unsafe { libc::getuid() };
    let mut command = Command::new("launchctl");
    command.args(["print", &format!("gui/{uid}/{}", service.label)]);
    let deadline = Instant::now() + Duration::from_secs(5);
    match crate::platform::run_bounded(&mut command, [1 << 20, 1 << 16], deadline, &|| false) {
        Ok((status, out, _)) if status.success() => {
            launchd_pid(&String::from_utf8_lossy(&out)) == Some(std::process::id())
        }
        _ => false,
    }
}
/// Elsewhere no service manager keeps the worker alive (on Windows the session
/// host supervises it).
#[cfg(not(any(target_os = "linux", target_os = "macos")))]
fn current(_: &Service) -> bool {
    false
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{fs::File, time::Instant};

    #[test]
    fn systemd_unit_is_the_leaf_of_the_systemd_hierarchy() {
        // The user manager's slice; spelled apart because the source privacy
        // check reads "user", "@" and "1000.service" together as an address.
        let manager = format!("/user.slice/user-1000.slice/user{}1000.service", '@');
        let unified = format!("0::{manager}/app.slice/hgs-recovery.service\n");
        assert_eq!(systemd_unit(&unified), Some("hgs-recovery.service"));
        let hybrid = format!(
            "12:memory:{manager}\n1:name=systemd:{manager}/hgs-recovery.service\n\
             0::{manager}/hgs-recovery.service\n"
        );
        assert_eq!(systemd_unit(&hybrid), Some("hgs-recovery.service"));
        // A worker a client started from a login session or from the desktop's own unit.
        let ssh = "0::/user.slice/user-1000.slice/session-4.scope\n";
        assert_eq!(systemd_unit(ssh), Some("session-4.scope"));
        let desktop = format!("0::{manager}/app.slice/hgs-tray.service\n");
        assert_eq!(systemd_unit(&desktop), Some("hgs-tray.service"));
        // Only a controller hierarchy, or the root: no unit.
        assert_eq!(systemd_unit("4:cpu,cpuacct:/hgs-recovery.service\n"), None);
        assert_eq!(systemd_unit("0::/\n"), None);
        assert_eq!(systemd_unit(""), None);
    }

    #[test]
    fn launchd_pid_reads_only_the_jobs_own_process() {
        let running = "gui/501/com.hgdev.hgs-recovery = {\n\
            \tactive count = 1\n\
            \tpath = /Users/example/Library/LaunchAgents/com.hgdev.hgs-recovery.plist\n\
            \tstate = running\n\
            \targuments = {\n\t\t/Applications/Zerus.app/Contents/MacOS/hgs\n\t\trecovery\n\t\tworker\n\t}\n\
            \tenvironment = {\n\t\tpid = 7\n\t}\n\
            \tpid = 4242\n\
            \tlast exit code = 0\n\
            }\n";
        assert_eq!(launchd_pid(running), Some(4242));
        let scheduled = "gui/501/com.hgdev.hgs-recovery = {\n\
            \tstate = spawn scheduled\n\
            \tnested = {\n\t\tpid = 7\n\t}\n\
            \tlast exit code = 0\n\
            }\n";
        assert_eq!(launchd_pid(scheduled), None);
        assert_eq!(launchd_pid("gui/501/x = {\n\tpid = 0\n}\n"), None);
        assert_eq!(launchd_pid(""), None);
    }

    struct Fixture {
        _dir: tempfile::TempDir,
        lock: PathBuf,
        standby: PathBuf,
    }

    fn fixture() -> Fixture {
        let dir = tempfile::tempdir().unwrap();
        let (lock, standby) = (
            dir.path().join("worker.lock"),
            dir.path().join("service.lock"),
        );
        Fixture {
            _dir: dir,
            lock,
            standby,
        }
    }

    fn hold(path: &Path) -> FileLock {
        let file = File::options()
            .create(true)
            .truncate(false)
            .write(true)
            .open(path)
            .unwrap();
        file.try_lock_exclusive().unwrap();
        FileLock::held(file)
    }

    fn eventually(check: impl Fn() -> bool) {
        let deadline = Instant::now() + Duration::from_secs(5);
        while !check() {
            assert!(Instant::now() < deadline, "condition not reached");
            std::thread::sleep(Duration::from_millis(5));
        }
    }

    const POLL: Duration = Duration::from_millis(10);

    #[test]
    fn free_lock_is_taken_without_asking_the_service_manager() {
        let f = fixture();
        let owner = acquire(&f.lock, &f.standby, || panic!("not asked"), || false, POLL)
            .unwrap()
            .expect("free lock");
        assert!(crate::status::locked(&f.lock));
        assert!(!owner.yields());
        // The service's worker starts waiting: this worker gives way between rounds.
        let standby = hold(&f.standby);
        assert!(owner.yields());
        drop(standby);
        assert!(!owner.yields());
        drop(owner);
        assert!(!crate::status::locked(&f.lock));
    }

    #[test]
    fn other_worker_exits_while_the_lock_is_held() {
        let f = fixture();
        let _executor = hold(&f.lock);
        let mut asked = false;
        let owner = acquire(
            &f.lock,
            &f.standby,
            || {
                asked = true;
                false
            },
            || false,
            POLL,
        )
        .unwrap();
        assert!(owner.is_none());
        assert!(asked);
        assert!(!waiting(&f.standby));
    }

    #[test]
    fn other_worker_leaves_the_free_lock_to_a_waiting_service_worker() {
        let f = fixture();
        let _service = hold(&f.standby);
        let owner = acquire(&f.lock, &f.standby, || panic!("not asked"), || false, POLL).unwrap();
        assert!(owner.is_none());
        assert!(!crate::status::locked(&f.lock));
    }

    #[test]
    fn service_worker_waits_for_the_lock_instead_of_exiting() {
        let f = fixture();
        let executor = hold(&f.lock);
        let (lock, standby) = (f.lock.clone(), f.standby.clone());
        let waiter = std::thread::spawn(move || acquire(&lock, &standby, || true, || false, POLL));
        // While it waits it announces itself, and other workers give way to it.
        eventually(|| waiting(&f.standby));
        std::thread::sleep(POLL * 5);
        assert!(!waiter.is_finished());
        drop(executor);
        let owner = waiter
            .join()
            .unwrap()
            .unwrap()
            .expect("service worker owns the lock");
        assert!(crate::status::locked(&f.lock));
        assert!(waiting(&f.standby));
        assert!(!owner.yields());
        drop(owner);
        assert!(!crate::status::locked(&f.lock));
        assert!(!waiting(&f.standby));
    }

    #[test]
    fn waiting_service_worker_exits_when_its_executable_is_replaced() {
        let f = fixture();
        let _executor = hold(&f.lock);
        let replaced = std::sync::atomic::AtomicBool::new(false);
        std::thread::scope(|scope| {
            let waiter = scope.spawn(|| {
                acquire(
                    &f.lock,
                    &f.standby,
                    || true,
                    || replaced.load(std::sync::atomic::Ordering::SeqCst),
                    POLL,
                )
            });
            eventually(|| waiting(&f.standby));
            replaced.store(true, std::sync::atomic::Ordering::SeqCst);
            assert!(waiter.join().unwrap().unwrap().is_none());
        });
        // The service manager starts the new version, which waits in turn.
        assert!(!waiting(&f.standby));
        assert!(crate::status::locked(&f.lock));
    }
}
