//! A deliberately sanitized error, suitable for transmission and logs: it
//! never carries a path, URL, token or driver diagnostic. The texts that
//! Python also raises are Python's exact words.
use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ConnectorError(pub String);

impl ConnectorError {
    pub fn new(message: impl Into<String>) -> Self {
        Self(message.into())
    }
    pub fn message(&self) -> &str {
        &self.0
    }
}

impl fmt::Display for ConnectorError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for ConnectorError {}

pub type Result<T> = std::result::Result<T, ConnectorError>;

pub const INVALID_SERVER_URL: &str = "invalid server_url";
pub const INSECURE_SERVER_URL: &str =
    "server_url requires HTTPS; development HTTP is loopback-only";
pub const FOREIGN_IDENTITY: &str =
    "identity_url may only preserve the previous official relay binding";
pub const INVALID_TOKEN: &str = "invalid node_token";
pub const INVALID_HGS: &str = "invalid hgs_path";
pub const INVALID_INTERVAL: &str = "poll_interval must be at least one second";
pub const INVALID_STATE: &str = "invalid state_dir";
pub const NOT_AN_OBJECT: &str = "configuration must be a JSON object";
pub const PRIVATE_DIRECTORY: &str = "connector state_dir must be an owned private directory";
pub const UNSAFE_FILE: &str = "connector journal file is not safe";
pub const PRIVATE_FILE: &str = "connector journal file must be owned and private";
pub const IN_USE: &str = "connector journal is already in use";
pub const FOREIGN_BINDING: &str =
    "journal belongs to different relay credentials; preserve its existing configuration";
pub const CONFLICT: &str = "request_id conflicts with preserved native request identity";
pub const RETAINED_LIMIT: &str = "connector retained request limit reached";
// Rust-only refusals (README divergences e, f and i).
pub const SHARED_CONFIGURATION: &str =
    "connector configuration must be owned by this user and not writable by others";
pub const OTHER_USER_HOME: &str = "state_dir may not name another user's home directory";
pub const NON_ASCII_HOST: &str = "server_url host must be ASCII";
pub const NO_HOME: &str = "home directory is unknown";
// Startup failures whose Python form is a library exception.
pub const UNREADABLE_CONFIGURATION: &str = "connector configuration could not be read";
pub const INVALID_JSON: &str = "connector configuration is not valid JSON";
pub const STATE_UNAVAILABLE: &str = "connector state_dir could not be created";
pub const JOURNAL_UNAVAILABLE: &str = "connector journal is unavailable";
