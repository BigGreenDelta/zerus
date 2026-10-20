//! The outbound mobile connector: it polls a Zerus relay for phone requests,
//! runs them through native `hgs` and journals every outcome locally, so a
//! mutation never executes twice. This library holds the parts the binary and
//! its tests share; `services/mobile/connector/README.md` records where the
//! Rust connector deliberately differs from the Python one.

pub mod accounts;
#[cfg(any(windows, test))]
mod admission;
pub mod archives;
pub mod capabilities;
pub mod check;
pub mod cli;
pub mod config;
pub mod error;
pub mod execute;
pub mod files;
pub mod fleet;
pub mod fsguard;
pub mod git_status;
pub mod hook_health;
#[cfg(feature = "test-hooks")]
pub mod hooks;
pub mod http;
pub mod inspection;
pub mod journal;
pub mod launch;
pub mod media;
pub mod mutations;
pub mod native;
pub mod projects;
pub mod pybase64;
pub mod pyjson;
pub mod pyvalue;
pub mod request;
pub mod runtime;
pub mod session_notifications;
pub mod speech;
pub mod url;
pub mod validate;
#[cfg(windows)]
pub mod windows_management;

pub use error::ConnectorError;

use std::path::Path;
use std::rc::Rc;
use std::time::Duration;

/// The fixed startup failure line; the reason stays out of the log because it
/// can name private paths. `--check` reports the reason instead.
pub const STARTUP_FAILURE: &str =
    "Connector could not start; verify private configuration and journal availability";
/// The journal failed while running, which also stops the Python connector.
pub const JOURNAL_FAILURE: &str = "Connector stopped: the connector journal is unavailable";

/// A started connector: its configuration and the journal it holds.
pub struct Started {
    pub config: config::Config,
    pub journal: journal::Journal,
    /// Findings that do not stop it, logged as warnings.
    pub warnings: Vec<&'static str>,
    #[cfg(windows)]
    generation: String,
}

/// What `main()` does before the runtime: read and validate the
/// configuration, then open the journal at clock `now`.
pub fn start(
    path: &Path,
    allow_insecure_localhost: bool,
    environment: &config::Environment,
    now: f64,
) -> Result<Started, ConnectorError> {
    start_bound(path, allow_insecure_localhost, environment, now, None)
}

/// A management child must admit exactly the configuration its launcher saw.
/// The comparison and journal/control publication share the management guard.
pub fn start_bound(
    path: &Path,
    allow_insecure_localhost: bool,
    environment: &config::Environment,
    now: f64,
    expected_revision: Option<&str>,
) -> Result<Started, ConnectorError> {
    #[cfg(windows)]
    let _guard = if expected_revision.is_some() {
        windows_management::admission_guard(path)
    } else {
        windows_management::guard(path, false)
    }
    .map_err(|_| ConnectorError::new(STARTUP_FAILURE))?;
    let (text, findings) = config::read_file(path, fsguard::euid())?;
    let revision = zerus_relay::json::digest(text.as_bytes());
    if expected_revision.is_some_and(|expected| expected != revision) {
        return Err(ConnectorError::new(STARTUP_FAILURE));
    }
    if let Some(refusal) = findings.iter().find(|finding| finding.refusal) {
        return Err(ConnectorError::new(refusal.message));
    }
    let value = config::parse(&text)?;
    let config = config::validate(&value, allow_insecure_localhost, environment)?;
    let journal = journal::Journal::open(&config.state_dir, &config.binding, now)
        .map_err(|error| ConnectorError::new(error.message()))?;
    #[cfg(windows)]
    let generation = windows_management::prepare_running(&config.state_dir, &revision)
        .map_err(|_| ConnectorError::new(STARTUP_FAILURE))?;
    Ok(Started {
        config,
        journal,
        #[cfg(windows)]
        generation,
        warnings: findings.iter().map(|finding| finding.message).collect(),
    })
}

/// Seconds since the Unix epoch, as Python's `time.time()`.
pub fn now() -> f64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0.0, |elapsed| elapsed.as_secs_f64())
}

/// The default native timeout: Python's 45 s. A build with the non-default
/// `test-hooks` feature reads `ZERUS_CONNECTOR_TEST_NATIVE_TIMEOUT`; other
/// builds do not contain the variable's name at all.
pub fn native_timeout() -> f64 {
    #[cfg(feature = "test-hooks")]
    if let Some(seconds) = hooks::number("NATIVE_TIMEOUT") {
        return seconds;
    }
    native::DEFAULT_TIMEOUT
}

/// The factor that divides the connector's own retry spacing: 1, or
/// `ZERUS_CONNECTOR_TEST_CLOCK_SCALE` with the `test-hooks` feature.
pub fn clock_scale() -> f64 {
    #[cfg(feature = "test-hooks")]
    if let Some(scale) = hooks::number("CLOCK_SCALE") {
        return scale;
    }
    1.0
}

/// Runs a started connector until a stop signal and returns its exit
/// status: 0 after a graceful stop, 1 when the journal failed. A second
/// signal exits at once (divergence h).
pub fn run(started: Started) -> u8 {
    let Started {
        config,
        journal,
        #[cfg(windows)]
        generation,
        ..
    } = started;
    let roots = if config.transport.secure {
        let roots = http::system_roots();
        if roots.is_none() {
            runtime::log(
                "WARNING",
                "No trusted TLS root certificates were found; relay connections will fail",
            );
        }
        roots
    } else {
        None
    };
    let Ok(tokio) = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
    else {
        runtime::log("ERROR", STARTUP_FAILURE);
        return 1;
    };
    let settings = runtime::Settings {
        interval: config.poll_interval,
        binding: config.binding.clone(),
        default_timeout: native_timeout(),
        clock_scale: clock_scale(),
    };
    #[cfg(windows)]
    let stop_state = config.state_dir.clone();
    let local = tokio::task::LocalSet::new();
    let code = local.block_on(&tokio, async move {
        let relay = http::Relay::new(&config.transport, &config.token, roots);
        let native = native::Process::new(config.hgs_path.clone(), settings.default_timeout);
        let connector = Rc::new(runtime::Connector::new(
            native,
            journal,
            settings,
            Box::new(now),
        ));
        #[cfg(unix)]
        let result = serve(&connector, &relay, signals(&connector.stopping)).await;
        #[cfg(windows)]
        let result = serve(
            &connector,
            &relay,
            windows_management::signals(&connector.stopping, stop_state, generation),
        )
        .await;
        match result {
            Ok(()) => 0,
            Err(_) => {
                runtime::log("ERROR", JOURNAL_FAILURE);
                1
            }
        }
    });
    // Usage refreshes still running are dropped, which kills their children.
    {
        let _context = tokio.enter();
        drop(local);
    }
    // A resolver thread may be waiting on DNS; never wait on it.
    tokio.shutdown_timeout(Duration::from_millis(200));
    code
}

/// The heartbeat and request loops until a stop signal; the heartbeat ends
/// at once, the request in progress first finishes.
async fn serve(
    connector: &Rc<runtime::Connector<native::Process>>,
    relay: &http::Relay,
    signals: impl std::future::Future<Output = ()>,
) -> Result<(), execute::Fatal> {
    use futures_util::future::{self, Either};
    let heartbeat = runtime::until_stopped(
        &connector.stopping,
        future::join(connector.heartbeats(relay), connector.gateway(relay)),
    );
    let requests = async {
        let result = connector.requests(relay).await;
        // A journal failure ends the heartbeat too.
        connector.stopping.set();
        result
    };
    let work = future::join(heartbeat, requests);
    match future::select(std::pin::pin!(work), std::pin::pin!(signals)).await {
        Either::Left(((_, requests), _)) => requests,
        Either::Right(((), _)) => Ok(()),
    }
}

/// SIGTERM or SIGINT stops claiming; a second signal exits at once without
/// touching any child, leaving a running request for restart recovery.
#[cfg(unix)]
async fn signals(stopping: &runtime::Event) {
    use tokio::signal::unix::{signal, SignalKind};
    let (Ok(mut terminate), Ok(mut interrupt)) = (
        signal(SignalKind::terminate()),
        signal(SignalKind::interrupt()),
    ) else {
        return std::future::pending().await;
    };
    let mut received = 0;
    loop {
        let number = {
            let term = std::pin::pin!(terminate.recv());
            let int = std::pin::pin!(interrupt.recv());
            match futures_util::future::select(term, int).await {
                futures_util::future::Either::Left(_) => 15,
                futures_util::future::Either::Right(_) => 2,
            }
        };
        received += 1;
        if received > 1 {
            runtime::log(
                "WARNING",
                "Connector stopped at once; restart recovers a request in progress",
            );
            std::process::exit(128 + number);
        }
        runtime::log("INFO", "Connector stopping after the request in progress");
        stopping.set();
    }
}
