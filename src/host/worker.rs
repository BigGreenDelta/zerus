//! D14: the host supervises the recovery worker (`hgs.exe recovery worker` of the
//! stable CLI), because Windows has no user service manager. Supervision starts
//! when a client asks (`recovery::ensure_worker` sends `recovery_worker`, as the
//! Unix CLI starts its worker); the worker is then restarted after it exits,
//! with a delay that doubles from 1 s to 5 min and starts again at 1 s after a
//! run that lasted `STABLE`, and it is stopped with the host. Its process is in
//! the host's own job, so it never outlives the host either.
use super::session::lock;
use std::{
    io,
    sync::{Arc, Condvar, Mutex, PoisonError},
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

/// The first restart delay.
pub(crate) const FIRST_DELAY: Duration = Duration::from_secs(1);
/// The longest restart delay.
pub(crate) const MAX_DELAY: Duration = Duration::from_secs(300);
/// A run this long resets the delay.
pub(crate) const STABLE: Duration = Duration::from_secs(60);
/// How often a running worker is checked against a stop request.
const POLL: Duration = Duration::from_millis(200);

/// The stable CLI whose recovery worker the host runs (D2, D14): the file
/// that `hgs session-host install` staged in this user's profile. Unlike
/// `platform::stable_executable`, the environment that started the host has
/// no say.
#[cfg(windows)]
pub(crate) fn stable_cli() -> Result<std::path::PathBuf, String> {
    let cli = super::staging::Layout::current()
        .ok_or("no home directory: set USERPROFILE or HOME")?
        .cli;
    if !cli.is_file() {
        return Err(format!(
            "the stable hgs CLI {} is not installed; run hgs session-host install",
            cli.display()
        ));
    }
    Ok(cli)
}

/// Restart delays.
#[derive(Debug)]
pub(crate) struct Backoff {
    next: Duration,
}

impl Default for Backoff {
    fn default() -> Self {
        Self { next: FIRST_DELAY }
    }
}

impl Backoff {
    /// The delay before the next start, after a run that lasted `ran`.
    pub(crate) fn after(&mut self, ran: Duration) -> Duration {
        if ran >= STABLE {
            self.next = FIRST_DELAY;
        }
        let delay = self.next;
        self.next = (self.next * 2).min(MAX_DELAY);
        delay
    }
}

/// A started worker process.
pub(crate) trait Process: Send {
    /// Its exit code once it has exited, waiting at most `timeout`.
    fn wait(&mut self, timeout: Duration) -> io::Result<Option<u32>>;
    fn kill(&mut self);
}

type Stop = Arc<(Mutex<bool>, Condvar)>;

/// A supervised worker; `stop` ends it.
pub(crate) struct Worker {
    stop: Stop,
    thread: JoinHandle<()>,
}

impl Worker {
    /// Start `spawn`'s process now and again after each exit.
    pub(crate) fn start<F>(name: &str, spawn: F) -> io::Result<Self>
    where
        F: FnMut() -> io::Result<Box<dyn Process>> + Send + 'static,
    {
        let stop: Stop = Arc::new((Mutex::new(false), Condvar::new()));
        let flag = stop.clone();
        let thread = thread::Builder::new()
            .name(name.into())
            .spawn(move || supervise(&flag, spawn, Backoff::default()))?;
        Ok(Self { stop, thread })
    }

    /// End the worker and stop restarting it.
    pub(crate) fn stop(self) {
        *lock(&self.stop.0) = true;
        self.stop.1.notify_all();
        let _ = self.thread.join();
    }
}

fn stopped(stop: &Stop) -> bool {
    *lock(&stop.0)
}

fn supervise(
    stop: &Stop,
    mut spawn: impl FnMut() -> io::Result<Box<dyn Process>>,
    mut backoff: Backoff,
) {
    while !stopped(stop) {
        let started = Instant::now();
        if let Ok(mut process) = spawn() {
            loop {
                if stopped(stop) {
                    process.kill();
                    return;
                }
                match process.wait(POLL) {
                    Ok(None) => {}
                    Ok(Some(_)) | Err(_) => break,
                }
            }
        }
        let deadline = Instant::now() + backoff.after(started.elapsed());
        let mut flag = lock(&stop.0);
        while !*flag {
            let left = deadline.saturating_duration_since(Instant::now());
            if left.is_zero() {
                break;
            }
            flag = stop
                .1
                .wait_timeout(flag, left)
                .unwrap_or_else(PoisonError::into_inner)
                .0;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

    #[test]
    fn delays_double_from_one_second_to_five_minutes_and_reset_after_a_stable_run() {
        let mut backoff = Backoff::default();
        let quick = Duration::from_millis(10);
        let delays: Vec<u64> = (0..12).map(|_| backoff.after(quick).as_secs()).collect();
        assert_eq!(delays, [1, 2, 4, 8, 16, 32, 64, 128, 256, 300, 300, 300]);
        assert_eq!(backoff.after(STABLE), FIRST_DELAY);
        assert_eq!(backoff.after(quick), Duration::from_secs(2));
    }

    struct Fake {
        killed: Arc<AtomicBool>,
        lasts: Option<Duration>,
        started: Instant,
    }

    impl Process for Fake {
        fn wait(&mut self, timeout: Duration) -> io::Result<Option<u32>> {
            match self.lasts {
                Some(lasts) if self.started.elapsed() >= lasts => Ok(Some(1)),
                _ => {
                    thread::sleep(timeout.min(Duration::from_millis(5)));
                    Ok(None)
                }
            }
        }

        fn kill(&mut self) {
            self.killed.store(true, Ordering::SeqCst);
        }
    }

    #[test]
    fn a_worker_restarts_after_exits_and_stops_with_the_host() {
        // A process that exits at once is restarted after the first delay.
        let runs = Arc::new(AtomicUsize::new(0));
        let killed = Arc::new(AtomicBool::new(false));
        let worker = {
            let (runs, killed) = (runs.clone(), killed.clone());
            Worker::start("test-worker", move || {
                let run = runs.fetch_add(1, Ordering::SeqCst);
                Ok(Box::new(Fake {
                    killed: killed.clone(),
                    // The first run exits; the second keeps running.
                    lasts: (run == 0).then_some(Duration::ZERO),
                    started: Instant::now(),
                }) as Box<dyn Process>)
            })
            .unwrap()
        };
        let deadline = Instant::now() + Duration::from_secs(10);
        while runs.load(Ordering::SeqCst) < 2 {
            assert!(Instant::now() < deadline, "not restarted");
            thread::sleep(Duration::from_millis(20));
        }
        assert!(!killed.load(Ordering::SeqCst));
        worker.stop();
        assert!(killed.load(Ordering::SeqCst), "the running worker ends");
        assert_eq!(runs.load(Ordering::SeqCst), 2);

        // A failing start is retried too, and a stop during the delay is prompt.
        let attempts = Arc::new(AtomicUsize::new(0));
        let worker = {
            let attempts = attempts.clone();
            Worker::start("test-worker", move || {
                attempts.fetch_add(1, Ordering::SeqCst);
                Err(io::Error::other("cannot start"))
            })
            .unwrap()
        };
        while attempts.load(Ordering::SeqCst) < 1 {
            thread::sleep(Duration::from_millis(5));
        }
        let stopping = Instant::now();
        worker.stop();
        assert!(stopping.elapsed() < Duration::from_millis(500));
    }
}
