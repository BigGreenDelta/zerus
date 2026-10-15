//! The outbound mobile connector: it polls a Zerus relay for phone requests,
//! runs them through native `hgs` and journals every outcome locally, so a
//! mutation never executes twice. This library holds the parts the binary and
//! its tests share; `services/mobile/connector/README.md` records where the
//! Rust connector deliberately differs from the Python one.
#[cfg(not(unix))]
compile_error!("the mobile connector supports only Unix gateways (Linux and macOS)");

pub mod check;
pub mod cli;
pub mod config;
pub mod error;
pub mod fsguard;
#[cfg(feature = "test-hooks")]
pub mod hooks;
pub mod journal;
pub mod pyjson;
pub mod request;
pub mod url;

pub use error::ConnectorError;

use std::path::Path;

/// The fixed startup failure line; the reason stays out of the log because it
/// can name private paths. `--check` reports the reason instead.
pub const STARTUP_FAILURE: &str =
    "Connector could not start; verify private configuration and journal availability";

/// A started connector: its configuration and the journal it holds.
pub struct Started {
    pub config: config::Config,
    pub journal: journal::Journal,
    /// Findings that do not stop it, logged as warnings.
    pub warnings: Vec<&'static str>,
}

/// What `main()` does before the runtime: read and validate the
/// configuration, then open the journal at clock `now`.
pub fn start(
    path: &Path,
    allow_insecure_localhost: bool,
    environment: &config::Environment,
    now: f64,
) -> Result<Started, ConnectorError> {
    let (text, findings) = config::read_file(path, fsguard::euid())?;
    if let Some(refusal) = findings.iter().find(|finding| finding.refusal) {
        return Err(ConnectorError::new(refusal.message));
    }
    let value = config::parse(&text)?;
    let config = config::validate(&value, allow_insecure_localhost, environment)?;
    let journal = journal::Journal::open(&config.state_dir, &config.binding, now)
        .map_err(|error| ConnectorError::new(error.message()))?;
    Ok(Started {
        config,
        journal,
        warnings: findings.iter().map(|finding| finding.message).collect(),
    })
}

/// Seconds since the Unix epoch, as Python's `time.time()`.
pub fn now() -> f64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0.0, |elapsed| elapsed.as_secs_f64())
}
