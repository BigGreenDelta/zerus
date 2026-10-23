//! `connector.json`, read and validated as `Connector.__init__` does
//! (`connector.py:253-283`), in the same order and with Python's texts, plus
//! the reviewed refusals of README divergences e, f, i and k.
use crate::error::*;
use crate::pyjson::{self, Value};
use crate::url::{self, Transport};
#[cfg(unix)]
use std::fs::Metadata;
use std::io::Read;
#[cfg(unix)]
use std::os::unix::fs::MetadataExt;
use std::path::{Component, Path, PathBuf};

pub const DEFAULT_STATE: &str = ".local/state/hgs/mobile-connector";

#[derive(Clone)]
pub struct Config {
    /// Python's `self.server_url`: the transport URL as text.
    pub server_url: String,
    /// The spelling the binding keeps across a managed relay migration.
    pub identity_url: String,
    pub transport: Transport,
    pub token: String,
    pub hgs_path: String,
    pub poll_interval: f64,
    /// Absolute: relative paths resolve against the working directory.
    pub state_dir: PathBuf,
    /// `sha256(identity_url + "\0" + node_token)` in hex.
    pub binding: String,
    /// Present only for endpoint-encrypted workspaces. The bearer lives in
    /// this private profile, never in a legacy-readable connector config.
    pub encrypted_workspace: Option<PathBuf>,
}

/// The node token is never logged, so it never reaches `{:?}` either.
impl std::fmt::Debug for Config {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Config")
            .field("server_url", &self.server_url)
            .field("identity_url", &self.identity_url)
            .field("transport", &self.transport)
            .field("token", &"<redacted>")
            .field("hgs_path", &self.hgs_path)
            .field("poll_interval", &self.poll_interval)
            .field("state_dir", &self.state_dir)
            .field("binding", &self.binding)
            .field("encrypted_workspace", &self.encrypted_workspace.is_some())
            .finish()
    }
}

/// HOME and the working directory, passed explicitly so tests need no
/// process-wide state.
#[derive(Debug, Clone)]
pub struct Environment {
    pub home: Option<PathBuf>,
    pub cwd: PathBuf,
}

impl Environment {
    pub fn current() -> std::io::Result<Self> {
        // os.path.expanduser: HOME when set, even empty; otherwise the
        // password database, which std::env::home_dir also consults.
        #[allow(deprecated)]
        let home = std::env::var_os("HOME")
            .map(PathBuf::from)
            .or_else(std::env::home_dir);
        Ok(Self {
            home,
            cwd: std::env::current_dir()?,
        })
    }
}

/// A reviewed difference from Python that `--check` reports.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Finding {
    pub divergence: char,
    /// A refusal stops the connector; otherwise it is a warning.
    pub refusal: bool,
    pub message: &'static str,
}

/// Divergence e: `hgs_path` is executed, so only this user may change the file.
#[cfg(unix)]
pub fn file_findings(metadata: &Metadata, euid: u32) -> Vec<Finding> {
    let mut findings = Vec::new();
    if metadata.uid() != euid || metadata.mode() & 0o022 != 0 {
        findings.push(Finding {
            divergence: 'e',
            refusal: true,
            message: SHARED_CONFIGURATION,
        });
    }
    if metadata.mode() & 0o044 != 0 {
        findings.push(Finding {
            divergence: 'e',
            refusal: false,
            message: "connector configuration is readable by other users",
        });
    }
    findings
}

/// The file's text and its divergence-e findings. The ownership and mode come
/// from the opened file, so a symlinked configuration is judged by its target,
/// which Python also reads.
pub fn read_file(path: &Path, euid: u32) -> Result<(String, Vec<Finding>)> {
    let unreadable = || ConnectorError::new(UNREADABLE_CONFIGURATION);
    #[cfg(unix)]
    let mut file = std::fs::File::open(path).map_err(|_| unreadable())?;
    #[cfg(windows)]
    let mut file =
        zerus_windows_private::open_file(path, false, false).map_err(|_| unreadable())?;
    #[cfg(unix)]
    let findings = file_findings(&file.metadata().map_err(|_| unreadable())?, euid);
    #[cfg(windows)]
    let findings = {
        let _ = euid;
        zerus_windows_private::verify(&file, false).map_err(|_| unreadable())?;
        Vec::new()
    };
    let mut bytes = Vec::new();
    #[cfg(unix)]
    file.read_to_end(&mut bytes).map_err(|_| unreadable())?;
    #[cfg(windows)]
    {
        file.by_ref()
            .take(65_537)
            .read_to_end(&mut bytes)
            .map_err(|_| unreadable())?;
        if bytes.len() > 65_536 {
            return Err(unreadable());
        }
    }
    // Path.read_text() decodes strict UTF-8, refusing encoded surrogates too.
    let text = String::from_utf8(bytes).map_err(|_| ConnectorError::new(INVALID_JSON))?;
    Ok((text, findings))
}

/// `json.loads(text)`, which must give an object.
pub fn parse(text: &str) -> Result<Value> {
    let value = pyjson::parse(text).map_err(|_| ConnectorError::new(INVALID_JSON))?;
    if !matches!(value, Value::Object(_)) {
        return Err(ConnectorError::new(NOT_AN_OBJECT));
    }
    Ok(value)
}

fn string(value: Option<&Value>) -> Option<String> {
    match value {
        Some(Value::Str(text)) => text.as_str(),
        _ => None,
    }
}

/// `Connector.__init__` up to the journal, which [`crate::journal`] opens.
pub fn validate(
    config: &Value,
    allow_insecure_localhost: bool,
    environment: &Environment,
) -> Result<Config> {
    let server =
        string(config.get("server_url")).ok_or_else(|| ConnectorError::new(INVALID_SERVER_URL))?;
    let stored = url::checked_url(&server, allow_insecure_localhost)?;
    let explicit = match config.get("identity_url") {
        None | Some(Value::Null) => None,
        Some(value) => {
            let text =
                string(Some(value)).ok_or_else(|| ConnectorError::new(INVALID_SERVER_URL))?;
            Some(url::checked_url(&text, allow_insecure_localhost)?)
        }
    };
    let identity_url = url::identity_url(&stored, explicit.as_deref())?;
    let server_url = url::transport(&stored);
    let transport = Transport::new(&server_url)?;

    let encrypted_workspace = match config.get("encrypted_workspace") {
        None => None,
        Some(marker) => {
            let path = string(marker.get("state_dir"))
                .filter(|path| !path.is_empty() && !path.contains('\0'))
                .map(PathBuf::from)
                .filter(|path| path.is_absolute());
            if !matches!(marker,Value::Object(fields) if fields.len()==2)
                || marker.get("version") != Some(&Value::Int("1".into()))
                || path.is_none()
                || config.get("node_token").is_some()
            {
                return Err(ConnectorError::new(
                    "Invalid encrypted workspace configuration",
                ));
            }
            path
        }
    };
    let token = if encrypted_workspace.is_some() {
        String::new()
    } else {
        string(config.get("node_token"))
            .filter(|token| !token.is_empty() && token.bytes().all(|b| (33..=126).contains(&b)))
            .ok_or_else(|| ConnectorError::new(INVALID_TOKEN))?
    };
    let hgs_path = match config.get("hgs_path") {
        None => "hgs".to_owned(),
        value => string(value)
            .filter(|path| !path.is_empty() && !path.contains('\0'))
            .ok_or_else(|| ConnectorError::new(INVALID_HGS))?,
    };
    #[cfg(windows)]
    let hgs_path = {
        let _ = hgs_path;
        // Portable folders may move between starts. The bundle, not PATH or
        // a stale saved installation path, selects its matching native CLI.
        std::env::current_exe()
            .map_err(|_| ConnectorError::new(INVALID_HGS))?
            .parent()
            .ok_or_else(|| ConnectorError::new(INVALID_HGS))?
            .join("hgs.exe")
            .to_string_lossy()
            .into_owned()
    };
    let poll_interval = match config.get("poll_interval") {
        None => 5.0,
        // A boolean is no number here (divergence k).
        Some(Value::Int(digits)) => digits.parse::<f64>().unwrap_or(f64::INFINITY),
        Some(Value::Float(number)) => *number,
        Some(_) => f64::NAN,
    };
    if !poll_interval.is_finite() || poll_interval < 1.0 {
        return Err(ConnectorError::new(INVALID_INTERVAL));
    }
    let state_dir = state_dir(config.get("state_dir"), environment)?;
    let binding = zerus_relay::json::digest(format!("{identity_url}\0{token}"));
    Ok(Config {
        server_url,
        identity_url,
        transport,
        token,
        hgs_path,
        poll_interval,
        state_dir,
        binding,
        encrypted_workspace,
    })
}

/// Resolve a protected bearer only after the configuration marker is fully
/// validated. Shared by startup and the strictly read-only support report.
pub fn bind_workspace(config: &mut Config, path: &Path) -> Result<()> {
    let Some(private_dir) = &config.encrypted_workspace else {
        return Ok(());
    };
    if path.parent().map(|p| p.join("workspace-private")).as_ref() != Some(private_dir) {
        return Err(ConnectorError::new(
            "Invalid encrypted workspace configuration",
        ));
    }
    let profile = crate::workspace::load_active(private_dir)?;
    if profile.server_url != config.server_url || profile.state_dir != config.state_dir {
        return Err(ConnectorError::new(
            "Encrypted workspace configuration changed",
        ));
    }
    config.token = profile.node_token;
    config.binding = zerus_relay::json::digest(format!(
        "encrypted-workspace-v1\0{}\0{}\0{}",
        config.identity_url, profile.membership.workspace_id, profile.identity.device.device_id
    ));
    Ok(())
}

/// `config.get("state_dir") or <default>`, then `Path(state).expanduser()`.
/// `null` and `""` mean the default; any other non-string is refused, where
/// Python took every falsy value for the default (divergence k). `~user` is
/// refused (divergence f).
fn state_dir(value: Option<&Value>, environment: &Environment) -> Result<PathBuf> {
    let home = || {
        environment
            .home
            .clone()
            .ok_or_else(|| ConnectorError::new(NO_HOME))
    };
    let text = match value {
        None | Some(Value::Null) => None,
        Some(Value::Str(text)) if text.is_empty() => None,
        value => Some(string(value).ok_or_else(|| ConnectorError::new(INVALID_STATE))?),
    };
    let Some(text) = text else {
        return Ok(normalize(&home()?.join(DEFAULT_STATE)));
    };
    let path = Path::new(&text);
    let mut components = path.components();
    let expanded = match components.next() {
        Some(Component::Normal(first)) if first == "~" => home()?.join(components.as_path()),
        Some(Component::Normal(first)) if first.as_encoded_bytes().starts_with(b"~") => {
            return Err(ConnectorError::new(OTHER_USER_HOME));
        }
        _ => path.to_path_buf(),
    };
    Ok(normalize(&environment.cwd.join(expanded)))
}

/// pathlib's lexical normal form: no `.` components or repeated separators.
fn normalize(path: &Path) -> PathBuf {
    path.components()
        .filter(|component| *component != Component::CurDir)
        .collect()
}

/// Divergences f, i and k in a parsed configuration, for `--check`; each also
/// fails [`validate`]. A key Python would refuse anyway is no divergence.
pub fn findings(config: &Value, allow_insecure_localhost: bool) -> Vec<Finding> {
    let mut findings = Vec::new();
    if config.get("poll_interval") == Some(&Value::Bool(true)) {
        findings.push(Finding {
            divergence: 'k',
            refusal: true,
            message: "poll_interval is a boolean, which the Python connector took for 1",
        });
    }
    match config.get("state_dir") {
        Some(
            value @ (Value::Bool(_)
            | Value::Int(_)
            | Value::Float(_)
            | Value::Array(_)
            | Value::Object(_)),
        ) if !value.truthy() => {
            findings.push(Finding {
                divergence: 'k',
                refusal: true,
                message: "state_dir is a falsy non-string, which the Python connector took for the default",
            });
        }
        Some(Value::Str(text)) => {
            let text = text.as_str().unwrap_or_default();
            let first = text.split('/').next().unwrap_or_default();
            if first.starts_with('~') && first != "~" {
                findings.push(Finding {
                    divergence: 'f',
                    refusal: true,
                    message: OTHER_USER_HOME,
                });
            }
        }
        _ => {}
    }
    let transport = string(config.get("server_url"))
        .and_then(|text| url::checked_url(&text, allow_insecure_localhost).ok())
        .map(|stored| url::transport(&stored));
    if let Some(Err(error)) = transport.map(|text| Transport::new(&text)) {
        if error.message() == NON_ASCII_HOST {
            findings.push(Finding {
                divergence: 'i',
                refusal: true,
                message: NON_ASCII_HOST,
            });
        }
    }
    findings
}

#[cfg(test)]
mod tests {
    use super::*;

    fn environment() -> Environment {
        Environment {
            home: Some("/home/example".into()),
            cwd: "/work".into(),
        }
    }

    fn state(text: &str) -> Result<PathBuf> {
        state_dir(Some(&pyjson::parse(text).unwrap()), &environment())
    }

    #[test]
    fn state_dir_expands_like_pathlib_and_refuses_other_homes() {
        assert_eq!(state("\"~\"").unwrap(), PathBuf::from("/home/example"));
        assert_eq!(
            state("\"~//a/./b/\"").unwrap(),
            PathBuf::from("/home/example/a/b")
        );
        assert_eq!(state("\"./rel\"").unwrap(), PathBuf::from("/work/rel"));
        assert_eq!(state("\"/abs/~x\"").unwrap(), PathBuf::from("/abs/~x"));
        assert_eq!(
            state("null").unwrap(),
            PathBuf::from("/home/example/.local/state/hgs/mobile-connector")
        );
        assert_eq!(
            state("\"~other/x\"").unwrap_err().message(),
            OTHER_USER_HOME
        );
        assert_eq!(state("\"~~\"").unwrap_err().message(), OTHER_USER_HOME);
        assert_eq!(state("false").unwrap_err().message(), INVALID_STATE);
        let homeless = Environment {
            home: None,
            cwd: "/work".into(),
        };
        assert_eq!(state_dir(None, &homeless).unwrap_err().message(), NO_HOME);
        assert_eq!(
            state_dir(Some(&pyjson::parse("\"rel\"").unwrap()), &homeless).unwrap(),
            PathBuf::from("/work/rel")
        );
    }

    #[test]
    fn debug_output_never_shows_the_token() {
        let config = pyjson::parse(
            "{\"server_url\": \"https://relay.example.test\", \"node_token\": \"fixture-node-token\"}",
        )
        .unwrap();
        let config = validate(&config, false, &environment()).unwrap();
        let shown = format!("{config:?}");
        assert!(!shown.contains("fixture-node-token"), "{shown}");
        assert!(shown.contains("<redacted>"), "{shown}");
    }

    #[test]
    fn findings_name_only_reviewed_divergences() {
        let divergences = |text: &str| -> Vec<char> {
            findings(&pyjson::parse(text).unwrap(), false)
                .iter()
                .map(|finding| finding.divergence)
                .collect()
        };
        assert_eq!(
            divergences("{\"poll_interval\": true, \"state_dir\": []}"),
            ['k', 'k']
        );
        assert_eq!(
            divergences("{\"poll_interval\": false, \"state_dir\": [1]}"),
            [] as [char; 0]
        );
        assert_eq!(divergences("{\"state_dir\": \"~root/x\"}"), ['f']);
        assert_eq!(
            divergences("{\"server_url\": \"https://r\\u00e9lay.example.test\"}"),
            ['i']
        );
        assert_eq!(
            divergences("{\"state_dir\": \"~/x\", \"server_url\": \"https://relay.example.test\"}"),
            [] as [char; 0]
        );
    }
}
